#include <log.h>
#include <stdio.h>
#include <stdint.h>

ssize_t newlib2uboot_write(int fd, const void *buf, size_t count) {
    assert(fd == 1 || fd == 2);
    for (size_t i = 0; i < count; ++i) {
        fputc(fd, ((const char *)buf)[i]);
    }
    return (ssize_t)count;
}
