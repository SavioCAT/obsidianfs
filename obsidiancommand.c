#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "ioctlops.h"

int main(int argc, char *argv[])
{
    int fd, ret;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error open(%s): %s\n", argv[1], strerror(errno));
        return 1;
    }

    ret = ioctl(fd, OBSIDIAN_TEST);
    if (ret < 0) {
        fprintf(stderr, "Error ioctl: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}