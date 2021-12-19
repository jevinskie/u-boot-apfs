// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

struct apple_mbox_msg {
	u64 msg0;
	u32 msg1;
};

int apple_rtkit_init(struct mbox_chan *);
int apple_rtkit_shutdown(struct mbox_chan *);
