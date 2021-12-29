// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021
 * Jevin Sweval, jevinsweval@gmail.com.
 */

#include <common.h>
#include <command.h>

static int do_exit2(struct cmd_tbl *cmdtp, int flag, int argc,
           char *const argv[])
{
    int r;

    r = 0;
    if (argc > 1)
        r = simple_strtoul(argv[1], NULL, 10);

    return -r - 2;
}

U_BOOT_CMD(
    exit2,   2,  1,  do_exit2,
    "exit2 script",
    ""
);
