#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/module.h>
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

static int DRV_major_num = 0;
static struct gendisk *DRV_disk;
static struct queue_limits limit;
static struct blk_mq_tag_set tag_set;

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
DEFINE_MUTEX(read_mtx); // xarray 읽기 뮤텍스
DEFINE_MUTEX(write_mtx); // xarray 쓰기 뮤텍스
DEFINE_MUTEX(flist_mtx); // free list 추가/수정/삭제 뮤텍스


#define             IOCTL_MAGIC         'G'
#define             DISPLAY_INDEX       _IOR(IOCTL_MAGIC, 2, struct display_info)
#define             GET_COUNT           _IOR(IOCTL_MAGIC, 3, struct display_info)

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

int pop_free_list(u32 *pba);


// Device Operations
static const struct blk_mq_ops bdops =
{
    .queue_rq = DRV_request, // 실제 I/O 연산 처리
    // .timeout = DRV_timeout // 타임아웃 처리
};

static const struct block_device_operations fops = {
    .ioctl = DRV_ioctl
};

// Entry Function
int DRV_init_module(void)
{
    // 요청 처리에 필요한 값 세팅 (tag_set)
    tag_set.ops = &bdops;
    tag_set.nr_hw_queues = 1; // 하드웨어 큐 개수
    tag_set.queue_depth = 128; // 최대 동시 요청 수
    
    if (blk_mq_alloc_tag_set(&tag_set))
    {
        pr_err("Tag set allocation failed\n");
        return -ENOMEM;
    }

    if ((DRV_data = vmalloc(DRV_LENGTH)) == NULL)
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
    // 디스크 등록 제거
    del_gendisk(DRV_disk); // 혹시 남을 I/O 요청을 안전하게 끝내기 위함
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
    int count = DRV_count_xa();

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

    // find xarray size
    xa_for_each(&xa, i, entry) count++;

    printk(KERN_INFO
       "[DISPLAY_INDEX] count=%d pba_ptr=%d\n",
       count, pba_ptr);

    unsigned long size = sizeof(*display_info) + count * sizeof(struct display_entry);
    display_info = kvmalloc(size, GFP_KERNEL);

    // kmalloc 실패 시
    if (!display_info) return -ENOMEM;

    int idx = 0;
    xa_for_each(&xa, i, entry) {
        display_info -> entries[idx].lba = i;
        display_info -> entries[idx].pba = xa_to_value(entry);
        idx++;
    }

    display_info->count = count;

    if (copy_to_user((void __user *)arg, display_info, size)) {
        kfree(display_info);
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
    int lba_left_len =  blk_rq_bytes(rq); // request 전체에서 읽어야 할 남은 바이트 기록
    int lba_offset = 0; // LBA 블록 내 어디까지 읽었는지 기록 (0 ~ 512B 사이의 값)
    
    mutex_lock(&read_mtx);

    // request 내 모든 bio_vec 세그먼트 순회
    rq_for_each_segment(bvec, rq, iter) {
        int bv_left_len = bvec.bv_len; // bvec가 얼마나 남았는지 기록
        int bv_offset = 0; // bvec 내 어디까지 썼는지 기록

        void *entry = xa_load(&xa, lba_ptr);

        // 읽기에 실패한 경우
        if (!entry) {
            printk("Error read pba data");
            mutex_unlock(&read_mtx);
            return -EFAULT;
        }

        // 공간 및 주소 할당
        void *kaddr = bvec_kmap_local(&bvec);

        // 현재 위치 계산
        u32 read_pba = xa_to_value(entry);
        char *pos = DRV_data + (read_pba * DRV_BLK_SIZE) + lba_offset;

        // LBA 데이터를 읽어서 bvec에 추가
        // 블록 값이 넘어가면 pba 값도 다시 계산 필요
        if (lba_left_len > DRV_BLK_SIZE - lba_offset) {

            if (bvec.bv_len < DRV_BLK_SIZE - lba_offset) {
                memcpy(kaddr+bv_offset, pos, bvec.bv_len);
                lba_left_len -= bvec.bv_len;
                bv_offset += bvec.bv_len;
                lba_offset += bvec.bv_len;
            } else {
                while (bv_left_len >= DRV_BLK_SIZE - lba_offset) {
                    int cpy_len = DRV_BLK_SIZE - lba_offset;
                    memcpy(kaddr+bv_offset, pos, cpy_len);

                    bv_offset += cpy_len;
                    bv_left_len -= cpy_len;
                    lba_left_len -= cpy_len;

                    lba_ptr++;
                    lba_offset = 0;

                    if (bv_left_len == 0) break; // 만약 bvec를 다 사용했다면 break

                    
                    entry = xa_load(&xa, lba_ptr);
                    // 읽기에 실패한 경우
                    if (!entry) {
                        printk("Error read pba data");
                        mutex_unlock(&read_mtx);
                        kunmap_local(kaddr);
                        return -EFAULT;
                    }
                    read_pba = xa_to_value(entry);
                    pos = DRV_data + (read_pba * DRV_BLK_SIZE);
                }
                if (bv_left_len > 0) {
                    memcpy(kaddr+bv_offset, pos, bv_left_len);

                    bv_offset += bv_left_len;
                    lba_offset += bv_left_len;
                    lba_left_len -= bv_left_len;

                    bv_left_len = 0;
                }
            }
        } else {
            if (bvec.bv_len < lba_left_len) {
                memcpy(kaddr+bv_offset, pos, bvec.bv_len);

                lba_offset += bvec.bv_len;
                lba_left_len -= bvec.bv_len;
                bv_offset += bv_left_len;
            } else { // LBA 내용 전부를 읽을 수 있음
                memcpy(kaddr+bv_offset, pos, lba_left_len);

                bv_offset += lba_left_len;
                lba_offset += lba_left_len;
                lba_left_len = 0;

                if (lba_offset == DRV_BLK_SIZE) {
                    lba_offset = 0;
                    lba_ptr++;
                }
            }
            
        }

        kunmap_local(kaddr);
    }
    mutex_unlock(&read_mtx);
    return 0;
}

int DRV_write(struct request *rq) {

    struct bio_vec bvec; // 물리 메모리 주소의 연속된 범위를 표현하는 구조체
    struct req_iterator iter;
    int lba_offset = 0; // 현재 PBA 블록에서 어디까지 썼는지 기록
    void *old_pba_entry;
    
    u32 lba_ptr = blk_rq_pos(rq); // 현재 섹터 위치 얻어오기
    u32 new_pba;

    mutex_lock(&write_mtx);
    // pbr 포인터 업데이트
    if (pba_ptr < DRV_TOTALBLK) {
        new_pba = pba_ptr++;
        mutex_unlock(&write_mtx);
    } else {
        int ret = pop_free_list(&new_pba);
        mutex_unlock(&write_mtx);
        if (ret) return ret;
    }

    // request 내 모든 bio_vec 세그먼트 순회
    rq_for_each_segment(bvec, rq, iter) {
        int bv_len = bvec.bv_len; // bvec가 얼마나 남았는지 기록
        int bv_offset = 0; // bvec 내 어디까지 썼는지 기록

        // 공간 및 주소 할당
        void *kaddr = bvec_kmap_local(&bvec);

        char *pos = DRV_data + (new_pba * DRV_BLK_SIZE) + lba_offset;

        if (bv_len < DRV_BLK_SIZE - lba_offset) {
            // bvec에서 읽어와서 pba 공간에 bvec.len만큼 추가
            memcpy(pos, kaddr+bv_offset, bv_len);
            lba_offset += bv_len; // 쓴 위치 기록
        } else {
            while (bv_len >= DRV_BLK_SIZE - lba_offset) {
                // 만약 xa_store 시 값이 있다면 기존 값 반환
                int cpy_len = DRV_BLK_SIZE - lba_offset;
                
                // 메모리 쓰기
                memcpy(pos, kaddr+bv_offset, cpy_len);
                bv_len -= cpy_len;
                bv_offset+= cpy_len;
                lba_offset += cpy_len;

                // 읽는 동안 L2P 매핑이 변화하면 안 됨
                mutex_lock(&read_mtx);
                old_pba_entry = xa_store(&xa, lba_ptr, xa_mk_value(new_pba), GFP_ATOMIC); // append index to xarray
                mutex_unlock(&read_mtx);

                if (old_pba_entry != NULL) { // overwrite인 경우
                    int old_pba = xa_to_value(old_pba_entry);
                    
                    struct free_list *new_node;
                    new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
                    if (!new_node) {
                        kunmap_local(kaddr);
                        return -ENOMEM;
                    }
                    new_node->idx = old_pba;
                    mutex_lock(&flist_mtx);
                    list_add(&new_node->node, &free_list_head); // stale node 추가
                    mutex_unlock(&flist_mtx);
                }

                lba_ptr++;
                lba_offset = 0;

                // 만약 bvec의 내용을 전부 읽었다면
                if(bv_len == 0) break;
                
                mutex_lock(&write_mtx);
                // pbr 포인터 업데이트
                if (pba_ptr < DRV_TOTALBLK) {
                    new_pba = pba_ptr++;
                    mutex_unlock(&write_mtx);
                } else {
                    int ret = pop_free_list(&new_pba);
                    mutex_unlock(&write_mtx);
                    if (ret) {
                        kunmap_local(kaddr);
                        return ret;
                    }
                }
                pos = DRV_data + (new_pba * DRV_BLK_SIZE);
            }

            // 만약 더 쓸 데이터가 남았다면
            if (bv_len > 0) {
                memcpy(pos, kaddr + bv_offset, bv_len);
                lba_offset += bv_len;
            }
        }

        kunmap_local(kaddr);
    }
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

module_init(DRV_init_module);
module_exit(DRV_cleanup_module);
MODULE_LICENSE("GPL");
