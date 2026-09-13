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
static int lba_ptr = 0;

/** L2P 매핑을 위한 XArray, free list 관리를 위한 Doubly Linked List 구현 **/
DEFINE_XARRAY(xa); // XArray 구현

#define DISPLAY_INDEX   1
#define GET_COUNT       2

static char *DRV_data;              // 블럭 디바이스 데이터 저장 공간

// free list 관리를 위한 변수
static LIST_HEAD(free_list_head);

// Function Prototype
int DRV_init_module(void);
void DRV_cleanup_module(void);
blk_status_t DRV_request(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd);
int DRV_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned cmd, unsigned long arg);
int DRV_write(struct request *rq);
int DRV_display_index(unsigned long arg);
int DRV_get_count_display(unsigned long arg);



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
    blk_status_t status = BLK_STS_OK;

    blk_mq_start_request(rq); // 타임아웃 타이머 시작

    switch(req_op(rq)) { // blk_opf_t : 연산 + 플래그 (REQ_OP_* | REQ_*)
    case REQ_OP_READ:
        printk("Test Success!!");
        break;
    case REQ_OP_WRITE:
        DRV_write(rq);
        break;
    }

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
    }
    return 0;
}

int DRV_get_count_display(unsigned long arg)
{
    int count = 0;
    unsigned long i;
    void *entry;

    // find xarray size
    xa_for_each(&xa, i, entry) count++;

    if (copy_to_user((void __user *)arg, &count, sizeof(count))) {
        return -EFAULT;
    }
    return 0;
}

int DRV_display_index(unsigned long arg)
{
    unsigned long i;
    void *entry;
    int count = 0;
    struct display_info *display_info;

    // find xarray size
    xa_for_each(&xa, i, entry) count++;

    unsigned long size = sizeof(*display_info) + count * sizeof(struct display_entry);
    display_info = kmalloc(size, GFP_KERNEL);

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

    kfree(display_info); // 할당 해제
    return 0;
}

int DRV_write(struct request *rq) {

    struct bio_vec bvec; // 물리 메모리 주소의 연속된 범위를 표현하는 구조체
    struct req_iterator iter;
    lba_ptr = blk_rq_pos(rq); // 현재 섹터 위치 얻어오기
    int data_len = 0;

    // request 내 모든 bio_vec 세그먼트 순회
    rq_for_each_segment(bvec, rq, iter) {

        // 현재 세그먼트(bvec)의 페이지 주소, 데이터 가져오기
        unsigned int len = bvec.bv_len;

        printk("[START] Writing data to the blk-mq device\n");
        void *kaddr = bvec_kmap_local(&bvec);
        memcpy(DRV_data + (pba_ptr * DRV_BLK_SIZE) + data_len, kaddr, len); // 데이터 저장
        data_len += len;
        printk("[END] Writing data to the blk-mq device\n");
        kunmap_local(kaddr);
    }
    
    int blk_count = blk_rq_sectors(rq);

    void *old_pba_entry; //xarray에 사용할 사용자 데이터 구조체

    // 순회하며 L2P 매핑 수행
    for (int x=0; x<blk_count; x++) {
        // 먄약 xa_store 시 값이 있다면 기존 값 반환
        old_pba_entry = xa_store(&xa, lba_ptr, xa_mk_value(pba_ptr), GFP_ATOMIC); // append index to xarray
        
        if (old_pba_entry != NULL) {
            int old_pba = xa_to_value(old_pba);
            struct free_list *new_node;
            new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
            if (!new_node) {
                return -ENOMEM;
            }
            new_node->idx = old_pba;
            list_add(&new_node->node, &free_list_head); // stack 형태로 stale node 추가

        }
        lba_ptr++;
        pba_ptr++;
    }

    return 0;
}

module_init(DRV_init_module);
module_exit(DRV_cleanup_module);
MODULE_LICENSE("GPL");
