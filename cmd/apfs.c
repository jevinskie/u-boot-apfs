// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021
 * Jevin Sweval, jevinsweval@gmail.com.
 */

#include <common.h>
#include <command.h>

extern void uboot_apfs_doit(void);

static int do_apfs(struct cmd_tbl *cmdtp, int flag, int argc,
           char *const argv[])
{
    printf("apfs\n");
    uboot_apfs_doit();
    printf("apfs done\n");

    return 0;
}

U_BOOT_CMD(
    apfs,   1,  1,  do_apfs,
    "apfs command",
    ""
);
