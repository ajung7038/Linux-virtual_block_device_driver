#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define             IOCTL_MAGIC         'G'
#define             DISPLAY_INDEX       _IOR(IOCTL_MAGIC, 2, struct display_info)
#define             GET_COUNT           _IOR(IOCTL_MAGIC, 3, int)

int main(void) {
    struct display_entry {
        unsigned int lba;
        unsigned int pba;
    };
    
    struct display_info {
        int count;
        struct display_entry entries[];
    };

    // 파일 오픈
    int fd = open("/dev/linux-disk", O_RDONLY);
    if (fd < 0) {
        perror("Cannot open /dev/linux-disk. Try again later\n");
        return 1;
    }

    // count 확인
    int count;
    if (ioctl(fd, GET_COUNT, &count) < 0) {
        printf("ERROR: Cannot found display index count");
        close(fd);
        return 1;
    }

    int size = sizeof(struct display_info) + 32768 * sizeof(struct display_entry);

    // 공간 할당
    struct display_info *info_data = malloc(size);

    // L2P 정보 얻기
    if (ioctl(fd, DISPLAY_INDEX, info_data) < 0) {
        printf("ERROR: display_index\n");
        free(info_data);
        close(fd);
        return 1;
    }

    
    // 정보 출력
    printf("=== display L2P index ===\n");
    printf("* sort by LBA index\n\n");
    printf("L : LBA index\nP : PBA index\n");
    printf("====================\n");
    printf("(L -> P)\n");
    printf("====================\n");

    for (int i=0; i<info_data->count; i++) {
        printf("(%u -> %u)\n", info_data->entries[i].lba, info_data->entries[i].pba);
    }

    printf("====================\n");
    printf("total: %d\n", info_data -> count);


    free(info_data); // 할당 해제

    if (close(fd) != 0) {
        printf("Cannot close\n");
    }
    return 0;
}
