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

int newlib2uboot_printf(const char * __restrict fmt, ...) {
  // return printf(fmt, __builtin_va_arg_pack());
    va_list args;
    va_start(args, fmt);
    int res = vprintf(fmt, args);
    va_end(args);
    return res;
}

__attribute__((naked))
void newlib2uboot_panic(const char * __restrict fmt, ...) {
    asm volatile ("b panic\n\t");
}
