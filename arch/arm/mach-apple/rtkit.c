// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <mailbox.h>
#include <malloc.h>

#include <asm/arch-apple/rtkit.h>

phys_addr_t apple_mbox_phys_start;
phys_addr_t apple_mbox_phys_addr;
phys_size_t apple_mbox_size;

int apple_rtkit_init(struct mbox_chan *chan)
{
	struct apple_mbox_msg msg;
	int endpoint;
	int endpoints[256];
	int nendpoints = 0;
	int msgtype;
	u64 subtype;
	int i, ret;

	if (apple_mbox_phys_start == 0) {
		apple_mbox_phys_start = (phys_addr_t)memalign(SZ_512K, SZ_64K);
		apple_mbox_phys_addr = apple_mbox_phys_start;
		apple_mbox_size = SZ_512K;
	}

	/* EP0_IDLE */
	msg.msg0 = 0x0060000000000220;
	msg.msg1 = 0x00000000;
	mbox_send(chan, &msg);

wait_hello:
	/* EP0_WAIT_HELLO */
	ret = mbox_recv(chan, &msg, 10000);

	endpoint = msg.msg1 & 0xff;
	msgtype = (msg.msg0 >> 52) & 0xff;
	if (endpoint != 0) {
		printf("%s: unexpected endpoint %d\n", __func__, endpoint);
		return -EINVAL;
	}
	if (msgtype == 6)
		goto wait_hello;
	if (msgtype != 1) {
		printf("%s: unexpected message type %d\n", __func__, msgtype);
		return -EINVAL;
	}

	/* EP0_SEND_HELLO */
	subtype = msg.msg0 & 0xffffffff;
	msg.msg0 = 0x0020000100000000 | subtype;
	msg.msg1 = 0x00000000;
	mbox_send(chan, &msg);

wait_epmap:
	/* EP0_WAIT_EPMAP */
	ret = mbox_recv(chan, &msg, 10000);

	endpoint = msg.msg1 & 0xff;
	msgtype = (msg.msg0 >> 52) & 0xff;
	if (endpoint != 0) {
		printf("%s: unexpected endpoint %d\n", __func__, endpoint);
		return -EINVAL;
	}
	if (msgtype != 8) {
		printf("%s: unexpected message type %d\n", __func__, msgtype);
		return -EINVAL;
	}

	for (i = 0; i < 32; i++) {
		if (msg.msg0 & (1U << i)) {
			endpoint = i + 32 * ((msg.msg0 >> 32) & 7);
			endpoints[nendpoints++] = endpoint;
		}
	}

	/* EP0_SEND_EPACK */
	subtype = (msg.msg0 >> 32) & 0x80007;
	msg.msg0 = 0x0080000000000000 | (subtype << 32) | !(subtype & 7);
	msg.msg1 = 0x00000000;
	mbox_send(chan, &msg);

	if ((subtype & 0x80000) == 0)
		goto wait_epmap;

	for (i = 0; i < nendpoints; i++) {
		/* EP0_SEND_EPSTART */
		subtype = endpoints[i];
		msg.msg0 = 0x0050000000000002 | (subtype << 32);
		msg.msg1 = 0x00000000;
		mbox_send(chan, &msg);
	}

	for (;;) {
		ret = mbox_recv(chan, &msg, 100000);
		if (ret == -ETIMEDOUT)
			break;
		if (ret < 0)
			return ret;

		endpoint = msg.msg1 & 0xff;
		msgtype = (msg.msg0 >> 52) & 0xff;

		if (endpoint == 0 && msgtype == 11)
			continue;

		if (endpoint == 1 || endpoint == 2 || endpoint == 4) {
			u64 size = (msg.msg0 >> 44) & 0xff;

			if (apple_mbox_phys_addr + (size << 12) >
			    apple_mbox_phys_start + apple_mbox_size) {
				printf("%s: out of memory\n", __func__);
				return -ENOMEM;
			}

			msg.msg0 &= ~0xfffffffffff;
			msg.msg0 |= apple_mbox_phys_addr;
			mbox_send(chan, &msg);
			apple_mbox_phys_addr += (size << 12);
			continue;
		}
		if (endpoint != 0) {
			printf("%s: unexpected endpoint %d\n", __func__, endpoint);
			return -EINVAL;
		}
		if (msgtype != 7) {
			printf("%s: unexpected message type %d\n", __func__, msgtype);
			return -EINVAL;
		}

		/* EP0_SEND_PWRACK */
		subtype = msg.msg0 & 0xffffffff;
		msg.msg0 = 0x00b0000000000000 | subtype;
		msg.msg1 = 0x00000000;
		mbox_send(chan, &msg);
	}

	return 0;
}
