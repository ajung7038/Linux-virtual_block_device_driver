#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/fs.h>

#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/init.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/memory.h>
#include <linux/uaccess.h>

#include <linux/xarray.h>
#include <linux/list.h>

#define DRV_NAME     "linux-driver" // 디바이스 드라이버 이름
#define DSK_NAME     "linux-disk" // 디스크 이름
#define DRV_LENGTH  (1024*1024*16) // 커널 내 가상 디스크 공간 크기 (16MB로 설정)
#define DRV_BLK_SIZE    512 // 블록 크기 (512B)
#define DRV_TOTALBLK    32768 // 전체 블록 개수

#define DRV_MAJOR       0 // 드라이버 major number
#define DRV_MINOR       0 // 드라이버 minor number

#define PERSIST_PATH "/var/lib/linux-device"

static int DRV_major_num = 0;
static struct gendisk *DRV_disk;
static struct queue_limits limit;
static struct blk_mq_tag_set tag_set;

static bool persist = true;

module_param(persist, bool, 0644); // 변수명, 타입, 권한
MODULE_PARM_DESC(persist, "Enable persist mode (default=true)");

static struct display_entry {
    u32 lba;
    u32 pba;
};

struct display_info {
    int count;
    struct display_entry entries[];
};

struct free_list {
    u32 idx; // pba idx
    struct list_head node;
};

/** pointer **/
static int pba_ptr = 0;

/** L2P 매핑을 위한 XArray 구현 **/
DEFINE_XARRAY(xa); // XArray 구현

/** Mutex 정의 **/
DEFINE_MUTEX(xa_mtx); // xarray 뮤텍스
DEFINE_MUTEX(write_mtx); // xarray 쓰기 뮤텍스
DEFINE_MUTEX(flist_mtx); // free list 추가/수정/삭제 뮤텍스
DEFINE_MUTEX(persist_mtx); // persist 옵션이 켜져 있을 경우

#define             IOCTL_MAGIC         'G'
#define             DISPLAY_INDEX       _IOR(IOCTL_MAGIC, 2, struct display_info)
#define             GET_COUNT           _IOR(IOCTL_MAGIC, 3, int)

static char *DRV_data;              // 블럭 디바이스 데이터 저장 공간

// free list 관리를 위한 변수
static LIST_HEAD(free_list_head);

// Function Prototype
int DRV_init_module(void);
void DRV_cleanup_module(void);

blk_status_t DRV_request(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd);
int DRV_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned cmd, unsigned long arg);
int DRV_write(struct request *rq);
int DRV_read(struct request *rq);

int DRV_display_index(unsigned long arg);
int DRV_get_count_display(unsigned long arg);
int DRV_count_xa(void);
static int DRV_get_pba(u32 lba, u32 *new_pba);

int pop_free_list(u32 *pba);


// Device Operations
static const struct blk_mq_ops bdops =
{
    .queue_rq = DRV_request, // 실제 I/O 연산 처리
};

static const struct block_device_operations fops = {
    .ioctl = DRV_ioctl
};

struct persist_save_xa {
    u32 lba;
    u32 pba;
};

struct persist_metadata {
    u32 pba_ptr;
    u32 xa_count;
    u32 lst_count;
};

// Entry Function
int DRV_init_module(void)
{
    // 요청 처리에 필요한 값 세팅 (tag_set)
    tag_set.ops = &bdops;
    tag_set.nr_hw_queues = 1; // 하드웨어 큐 개수
    tag_set.queue_depth = 128; // 최대 동시 요청 수
    tag_set.flags = BLK_MQ_F_BLOCKING; // 블로킹 가능한 큐임을 명시
    
    if (blk_mq_alloc_tag_set(&tag_set))
    {
        pr_err("Tag set allocation failed\n");
        return -ENOMEM;
    }

    if ((DRV_data = vzalloc(DRV_LENGTH)) == NULL)
    {
        blk_mq_free_tag_set(&tag_set); // tag_set 해제
        return -ENOMEM;
    }

    // 커널에 블록 디바이스 등록
    if ((DRV_major_num = register_blkdev(DRV_MAJOR, DRV_NAME)) < 0)
    {
        blk_mq_free_tag_set(&tag_set); // tag_set 해제
        vfree(DRV_data); // vmalloc으로 할당된 공간 해제

        printk(DRV_NAME " : Device registration failed (%d)\n", DRV_major_num);
        return DRV_major_num;
    }

    printk(DRV_NAME " : Device registered with Major Number = %d\n", DRV_major_num);

    // 만약 persist 옵션이 켜져 있다면 내용 불러오기
    if (persist) {
        struct file *persist_file;

        struct persist_metadata metadata;
        struct persist_save_xa map;
        struct free_list *node;

        int idx = 0;
        char *PERSIST_data = NULL;

        size_t size;
        ssize_t file_len;
        size_t offset = 0;

        loff_t file_offset = 0;
        
        int ret = 0;

        // 파일 열기
        mutex_lock(&persist_mtx);
        persist_file = filp_open(PERSIST_PATH, O_RDONLY, 0);

        if (!IS_ERR(persist_file)) {
            // 메타데이터 읽기 -> 나머지 size 파악
            file_len = kernel_read(persist_file, &metadata, sizeof(metadata), &file_offset);

            if (file_len != sizeof(metadata)) {
                filp_close(persist_file, NULL);
                mutex_unlock(&persist_mtx);

                vfree(DRV_data);
                blk_mq_free_tag_set(&tag_set);
                return -EFAULT;
            }

            size = sizeof(struct persist_metadata) + DRV_LENGTH + metadata.xa_count * sizeof(struct persist_save_xa) + metadata.lst_count * sizeof(u32);

            PERSIST_data = vmalloc(size);
            if (!PERSIST_data) {
                filp_close(persist_file, NULL);
                mutex_unlock(&persist_mtx);

                vfree(DRV_data);
                blk_mq_free_tag_set(&tag_set);

                return -ENOMEM;
            }

            memcpy(PERSIST_data, &metadata, sizeof(metadata));

            // 메타데이터 빼고 나머지 부분 읽기
            file_len = kernel_read(persist_file, PERSIST_data + sizeof(metadata), size - sizeof(metadata), &file_offset);

            filp_close(persist_file, NULL);
            offset = sizeof(struct persist_metadata);

            // DRV_data 복사
            memcpy(DRV_data, PERSIST_data + offset, DRV_LENGTH);
            offset += DRV_LENGTH;

            // XArray 복사
            for (idx = 0; idx < metadata.xa_count; idx++) {
                memcpy(&map, PERSIST_data + offset, sizeof(map));
                offset += sizeof(map);

                xa_store(&xa, map.lba, xa_mk_value(map.pba), GFP_KERNEL);
            }

            // XArray가 존재해야만 free list가 존재할 수 있으므로
            if (!ret) {
                for (idx = 0; idx < metadata.lst_count; idx++) {
                    u32 old_pba_ptr;

                    memcpy(&old_pba_ptr, PERSIST_data+offset, sizeof(old_pba_ptr));
                    offset += sizeof(old_pba_ptr);

                    node = kmalloc(sizeof(*node), GFP_KERNEL);

                    if (!node) break; // 공간이 부족해질 경우 할당 중단

                    node->idx = old_pba_ptr;

                    list_add(&node -> node, &free_list_head); // 돌면서 리스트에 추가
                }
            }

            pba_ptr = metadata.pba_ptr;
            vfree(PERSIST_data);
        }
        mutex_unlock(&persist_mtx);
    }

    DRV_disk = blk_mq_alloc_disk(&tag_set, &limit, DRV_data); // 내부적으로 큐도 같이 생성
    
    // 만약 디스크 할당에 실패한다면
    if (IS_ERR(DRV_disk)) {
        unregister_blkdev(DRV_major_num, DRV_NAME);
        vfree(DRV_data); // vmalloc으로 할당된 공간 해제
        blk_mq_free_tag_set(&tag_set); // tag_set 해제

        return PTR_ERR(DRV_disk);
    }
        
    set_capacity(DRV_disk, DRV_TOTALBLK);

    // 주번호, 부번호 설정
    strscpy(DRV_disk -> disk_name, DSK_NAME); // char* 형으로 전환
    DRV_disk -> major = DRV_major_num;
    DRV_disk -> first_minor = DRV_MINOR;
    DRV_disk -> fops = &fops;
    DRV_disk -> minors = 1; // gendisk 구조체에 할당할 minor 번호가 몇 개인지 정함
    
    int result = add_disk(DRV_disk);
    if (result < 0) // 만약 disk 추가에 실패했다면 역순으로 free
    {
        // tag_set -> vmalloc -> register_blkdev -> blk_mq_alloc_disk 전부 돌리기
        // DRV_cleanup_module 이후 과정과 동일 (del_gendisk 제외)
        put_disk(DRV_disk);
        unregister_blkdev(DRV_major_num, DRV_NAME);
        vfree(DRV_data); // vmalloc으로 할당된 공간 해제
        blk_mq_free_tag_set(&tag_set); // tag_set 해제
    }
    
    return result; // 실패 시 음수 반환
}

void DRV_cleanup_module(void)
{
    // 만약 persist 옵션이 켜져 있다면
    if (persist) {
        unsigned long i;
        void *entry;

        int lst_count = 0;

        // 파일 열어서 공간에 접근
        size_t size;
        int offset = 0;
        
        struct persist_metadata metadata;
        struct free_list *node;
        char *PERSIST_data;

        struct file *persist_file; // 내용을 쓸 파일
        loff_t file_offset = 0;
        ssize_t file_len;

        mutex_lock(&persist_mtx);
        int xa_count = DRV_count_xa();

        
        if (!list_empty(&free_list_head)) {
            list_for_each_entry(node, &free_list_head, node) lst_count++;
        }

        size = sizeof(struct persist_metadata) + DRV_LENGTH + xa_count * sizeof(struct persist_save_xa) + lst_count * sizeof(u32);

        PERSIST_data = vmalloc(size);
        if (!PERSIST_data) {
            mutex_unlock(&persist_mtx);
            // 할당된 공간을 차례대로 해제
            del_gendisk(DRV_disk); // 혹시 남을 I/O 요청을 안전하게 끝내기 위함
            put_disk(DRV_disk);
            unregister_blkdev(DRV_major_num, DRV_NAME);
            blk_mq_free_tag_set(&tag_set);
            vfree(DRV_data);
            return;
        }

        metadata.pba_ptr = pba_ptr;
        metadata.xa_count = xa_count;
        metadata.lst_count = lst_count;

        // 메타데이터 저장
        memcpy(PERSIST_data + offset, &metadata, sizeof(metadata));
        offset += sizeof(metadata);

        // 데이터 (16MB) 저장
        memcpy(PERSIST_data + offset, DRV_data, DRV_LENGTH);
        offset += DRV_LENGTH;

        // XArray 저장 (저장하는 동안 다른 프로세스/스레드가 XArray를 변화하지 못하게 해야함)
        xa_for_each(&xa, i, entry) {
            struct persist_save_xa map;
            map.lba = i;
            map.pba = xa_to_value(entry);
            memcpy(PERSIST_data + offset, &map, sizeof(map));
            offset += sizeof(map);
        };

        // free list 저장
        list_for_each_entry(node, &free_list_head, node) {
            u32 old_pba_ptr = node -> idx;
            memcpy(PERSIST_data+offset, &old_pba_ptr, sizeof(old_pba_ptr));
            offset += sizeof(old_pba_ptr);
        }

        persist_file = filp_open(PERSIST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (IS_ERR(persist_file)) { // 파일을 열지 못했다면
            vfree(PERSIST_data);
            mutex_unlock(&persist_mtx);
            // 할당된 공간을 차례대로 해제
            del_gendisk(DRV_disk); // 혹시 남을 I/O 요청을 안전하게 끝내기 위함
            put_disk(DRV_disk);
            unregister_blkdev(DRV_major_num, DRV_NAME);
            blk_mq_free_tag_set(&tag_set);
            vfree(DRV_data);
            return;
        }

        file_len = kernel_write(persist_file, PERSIST_data, size, &file_offset);
        filp_close(persist_file, NULL);


        vfree(PERSIST_data);
        mutex_unlock(&persist_mtx);
    }

    // 디스크 등록 제거
    del_gendisk(DRV_disk); // 혹시 남아 있을 I/O 요청을 안전하게 끝내기 위함
    put_disk(DRV_disk);

    // 블럭 디바이스 해제
    unregister_blkdev(DRV_major_num, DRV_NAME);

    // tag_set 해제
    blk_mq_free_tag_set(&tag_set);

    // 장치 저장 공간 해제
    vfree(DRV_data);
}


blk_status_t DRV_request(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd -> rq;
    blk_status_t status;
    int ret = 0;

    blk_mq_start_request(rq); // 타임아웃 타이머 시작

    switch(req_op(rq)) { // blk_opf_t : 연산 + 플래그 (REQ_OP_* | REQ_*)
    case REQ_OP_READ:
        ret = DRV_read(rq);
        break;
    case REQ_OP_WRITE:
        ret = DRV_write(rq);
        break;
    default:
        ret = -EOPNOTSUPP;
        break;
    }

    status = errno_to_blk_status(ret);

    // rq가 끝났음을 알리기
    blk_mq_end_request(rq, status);
    return BLK_STS_OK;
}

int DRV_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned cmd, unsigned long arg)
{
    switch(cmd) {
    case DISPLAY_INDEX:
        DRV_display_index(arg);
        break;
    case GET_COUNT:
        DRV_get_count_display(arg);
        break;
    default:
        return -ENOTTY;
    }
    return 0;
}

int DRV_get_count_display(unsigned long arg)
{
    mutex_lock(&xa_mtx);
    int count = DRV_count_xa();
    mutex_unlock(&xa_mtx);

    printk(KERN_INFO "[GET_COUNT] count=%d pba_ptr=%d\n",count, pba_ptr);

    if (copy_to_user((void __user *)arg, &count, sizeof(count))) {
        return -EFAULT;
    }
    return 0;
}

int DRV_count_xa(void) {
    int count = 0;
    unsigned long i;
    void *entry;

    // find xarray size
    xa_for_each(&xa, i, entry) count++;
    return count;
}

int DRV_display_index(unsigned long arg)
{
    unsigned long i;
    void *entry;
    int count = 0;
    struct display_info *display_info;

    mutex_lock(&xa_mtx);
    // find xarray size
    xa_for_each(&xa, i, entry) count++;

    unsigned long size = sizeof(*display_info) + count * sizeof(struct display_entry);
    display_info = kvmalloc(size, GFP_KERNEL);

    // kmalloc 실패 시
    if (!display_info) {
        mutex_unlock(&xa_mtx);
        return -ENOMEM;
    }

    int idx = 0;
    xa_for_each(&xa, i, entry) {
        display_info -> entries[idx].lba = i;
        display_info -> entries[idx].pba = xa_to_value(entry);
        idx++;
    }

    display_info->count = count;
    mutex_unlock(&xa_mtx);

    if (copy_to_user((void __user *)arg, display_info, size)) {
        kvfree(display_info);
        return -EFAULT;
    }

    kvfree(display_info); // 할당 해제
    return 0;
}

int DRV_read(struct request *rq)
{
    struct bio_vec bvec; // 물리 메모리 주소의 연속된 범위를 표현하는 구조체
    struct req_iterator iter;
    u32 lba_ptr = blk_rq_pos(rq); // 현재 섹터 위치 얻어오기
    int lba_offset = 0; // LBA 블록 내 어디까지 읽었는지 기록 (0 ~ 512B 사이의 값)
    
    mutex_lock(&xa_mtx);

    // request 내 모든 bio_vec 세그먼트 순회
    rq_for_each_segment(bvec, rq, iter) {
        int bv_left_len = bvec.bv_len; // bvec가 얼마나 남았는지 기록
        int bv_offset = 0; // bvec 내 어디까지 썼는지 기록
        void *kaddr = bvec_kmap_local(&bvec); // 공간 및 주소 할당

        while (bv_left_len > 0) {
            int data = min(bv_left_len, DRV_BLK_SIZE - lba_offset);

            void *entry = xa_load(&xa, lba_ptr);

            // 읽을 데이터가 없는 경우 0 반환
            if (!entry) {
                memset(kaddr + bv_offset, 0, data);
            } else {
                // 현재 위치 계산
                u32 read_pba = xa_to_value(entry);
                char *pos = DRV_data + (read_pba * DRV_BLK_SIZE) + lba_offset;
                memcpy(kaddr+bv_offset, pos, data);
            }

            bv_left_len -= data;
            bv_offset += data;
            lba_offset += data;

            if (lba_offset == DRV_BLK_SIZE) {
                lba_ptr++;
                lba_offset = 0;
            }
        }
        kunmap_local(kaddr);
    }
    mutex_unlock(&xa_mtx);
    return 0;
}

int DRV_write(struct request *rq) {
    struct bio_vec bvec; // 물리 메모리 주소의 연속된 범위를 표현하는 구조체
    struct req_iterator iter;

    u32 lba_ptr = blk_rq_pos(rq); // 현재 섹터 위치 얻어오기
    int rq_byte_left =  blk_rq_bytes(rq); // 읽어야 할 데이터 양 (바이트)
    int lba_offset = 0; // 현재 PBA 블록에서 어디까지 썼는지 기록
    u32 new_pba;
    int ret;

    mutex_lock(&persist_mtx);
    mutex_lock(&xa_mtx);
    // 첫 번째 블록만 블록 사용 여부 검사
    ret = DRV_get_pba(lba_ptr, &new_pba);


    if (ret == -ENOSPC) {
        mutex_unlock(&xa_mtx);
        mutex_unlock(&persist_mtx);
        return ret;
    }

    // request 내 모든 bio_vec 세그먼트 순회
    rq_for_each_segment(bvec, rq, iter) {
        int bv_len_left = bvec.bv_len; // bvec가 얼마나 남았는지 기록
        int bv_offset = 0; // bvec 내 어디까지 썼는지 기록

        // 공간 및 주소 할당
        void *kaddr = bvec_kmap_local(&bvec);

        // bio vec를 다 읽을 때까지 반복
        while (bv_len_left > 0) {
            char *pos = DRV_data + (new_pba * DRV_BLK_SIZE) + lba_offset;
            int data = min(bv_len_left, DRV_BLK_SIZE - lba_offset);

            // 메모리 쓰기
            memcpy(pos, kaddr+bv_offset, data);
            bv_len_left -= data;
            bv_offset+= data;
            lba_offset += data;
            rq_byte_left -= data;

            // PBA 공간에 더 쓸 수 있다면 다음 bio_vec으로 이동
            if (lba_offset < DRV_BLK_SIZE) break;

            void *old_pba_entry;
            old_pba_entry = xa_store(&xa, lba_ptr, xa_mk_value(new_pba), GFP_ATOMIC); // append index to xarray

            if (old_pba_entry != NULL) { // overwrite된 블록이라면
                int old_pba = xa_to_value(old_pba_entry);

                if (old_pba != new_pba) { // 이미 업데이트되어 있지 않으면 (가득 차서 미리 free list로 넣은 게 아니라면)
                    struct free_list *new_node;
                    new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
                    if (!new_node) {
                        mutex_unlock(&xa_mtx);
                        mutex_unlock(&persist_mtx);
                        kunmap_local(kaddr);
                        return -ENOMEM;
                    }
                    new_node->idx = old_pba;
                    mutex_lock(&flist_mtx);
                    list_add(&new_node->node, &free_list_head); // stale node 추가
                    mutex_unlock(&flist_mtx);
                }
            }
            
            lba_ptr++;
            lba_offset = 0;

            if (rq_byte_left == 0) break; // 처리가 끝났다면 나가기

            // PBA 업데이트
            ret = DRV_get_pba(lba_ptr, &new_pba);

            if (ret == -ENOSPC) {
                kunmap_local(kaddr);
                mutex_unlock(&xa_mtx);
                mutex_unlock(&persist_mtx);
                return ret;
            }
        }
        kunmap_local(kaddr);
    }
    mutex_unlock(&xa_mtx);
    mutex_unlock(&persist_mtx);
    return 0;
}

int pop_free_list(u32 *pba) {
    struct free_list *node;

    mutex_lock(&flist_mtx);

    // 만약 free list가 비어있다면
    if (list_empty(&free_list_head)) {
        mutex_unlock(&flist_mtx);
        return -ENOSPC;
    }
    
    node = list_first_entry(&free_list_head, struct free_list, node);
    *pba = node -> idx;
    list_del(&node -> node); // 꺼낸 노드 제거
    
    mutex_unlock(&flist_mtx);
    
    kfree(node);

    return 0;
}

int DRV_get_pba(u32 lba, u32 *new_pba)
{
    int ret;
    void *entry;
    
    // 만약 아직 pba 공간이 남아있다면
    mutex_lock(&write_mtx);
    if (pba_ptr < DRV_TOTALBLK) {
        *new_pba = pba_ptr++;
         mutex_unlock(&write_mtx);
         return 0;
    }
    mutex_unlock(&write_mtx);

    // 만약 pba 공간을 사용했다면 free list에서 pop하기
    ret = pop_free_list(new_pba);

    // free list 공간이 가득 찼다면
    if (ret == -ENOSPC) {
        // 이미 쓸 공간이 없다면 기존에 사용하던 공간을 free list에 등록한 후 append 필요
        entry = xa_load(&xa, lba);
        if (!entry) return -ENOSPC;
        *new_pba = xa_to_value(entry);
    }
    return 0;
}

module_init(DRV_init_module);
module_exit(DRV_cleanup_module);
MODULE_LICENSE("GPL");
