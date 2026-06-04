#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "ioctlops.h"

int main(int argc, char *argv[])
{
    int fd, ret;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <-p|-l> <file>\n", argv[0]);
        return 1;
    }

    fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error open(%s): %s\n", argv[2], strerror(errno));
        return 1;
    }

    if (strcmp(argv[1], "-p") == 0) {
        ret = ioctl(fd, OBSIDIAN_IOC_PROTECT);
        if (ret < 0) {
            fprintf(stderr, "Error ioctl PROTECT: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        printf("inode protected\n");
    } else if (strcmp(argv[1], "-l") == 0) {
        ret = ioctl(fd, OBSIDIAN_IOC_DENTRY_TEST);
        if (ret < 0) {
            fprintf(stderr, "Error ioctl DENTRY_TEST: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
    } else {
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
