// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

struct apple_mbox_msg {
	u64 msg0;
	u32 msg1;
};

extern phys_addr_t apple_rtkit_phys_start;
extern phys_addr_t apple_rtkit_phys_addr;

int apple_rtkit_init(struct mbox_chan *, int,
		     int (*)(void *, phys_addr_t, phys_size_t), void *);
