// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 * (C) Copyright 2021 Copyright The Asahi Linux Contributors
 */

#include <common.h>
#include <mailbox.h>
#include <malloc.h>

#include <asm/arch-apple/rtkit.h>
#include <linux/bitfield.h>

#define APPLE_RTKIT_EP_MGMT 0
#define APPLE_RTKIT_EP_CRASHLOG	1
#define APPLE_RTKIT_EP_SYSLOG 2
#define APPLE_RTKIT_EP_DEBUG 3
#define APPLE_RTKIT_EP_IOREPORT 4

#define APPLE_RTKIT_MGMT_TYPE GENMASK(59, 52)

#define APPLE_RTKIT_MGMT_HELLO 1
#define APPLE_RTKIT_MGMT_HELLO_REPLY 2
#define APPLE_RTKIT_MGMT_HELLO_MINVER GENMASK(15, 0)
#define APPLE_RTKIT_MGMT_HELLO_MAXVER GENMASK(31, 16)

#define APPLE_RTKIT_MGMT_STARTEP 5
#define APPLE_RTKIT_MGMT_STARTEP_EP GENMASK(39, 32)
#define APPLE_RTKIT_MGMT_STARTEP_FLAG BIT(1)

#define APPLE_RTKIT_MGMT_PWR_STATE_ACK 7

#define APPLE_RTKIT_MGMT_EPMAP 8
#define APPLE_RTKIT_MGMT_EPMAP_LAST BIT(51)
#define APPLE_RTKIT_MGMT_EPMAP_BASE GENMASK(34, 32)
#define APPLE_RTKIT_MGMT_EPMAP_BITMAP GENMASK(31, 0)

#define APPLE_RTKIT_MGMT_EPMAP_REPLY 8
#define APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE BIT(0)

#define APPLE_RTKIT_MIN_SUPPORTED_VERSION 11
#define APPLE_RTKIT_MAX_SUPPORTED_VERSION 12

/* Messages for internal endpoints. */
#define APPLE_RTKIT_BUFFER_REQUEST 1

phys_addr_t apple_rtkit_phys_start;
phys_addr_t apple_rtkit_phys_addr;
phys_size_t apple_rtkit_size;

int apple_rtkit_init(struct mbox_chan *chan, int wakeup,
		     int (*sart_map)(void *, phys_addr_t, phys_size_t),
		     void *cookie)
{
	struct apple_mbox_msg msg;
	int endpoint;
	int endpoints[256];
	int nendpoints = 0;
	int min_ver, max_ver, want_ver;
	int msgtype;
	u64 reply;
	u32 bitmap, base;
	int i, ret;

	if (apple_rtkit_phys_start == 0) {
		apple_rtkit_phys_start = (phys_addr_t)memalign(SZ_16K, SZ_256K);
		apple_rtkit_phys_addr = apple_rtkit_phys_start;
		apple_rtkit_size = SZ_256K;
	}

	/* EP0_IDLE */
	if (wakeup) {
		msg.msg0 = 0x0060000000000220;
		msg.msg1 = APPLE_RTKIT_EP_MGMT;
		mbox_send(chan, &msg);
	}

	/* EP0_WAIT_HELLO */
	ret = mbox_recv(chan, &msg, 10000);

	endpoint = msg.msg1;
	msgtype = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg.msg0);
	if (endpoint != APPLE_RTKIT_EP_MGMT) {
		printf("%s: unexpected endpoint %d\n", __func__, endpoint);
		return -EINVAL;
	}
	if (msgtype != APPLE_RTKIT_MGMT_HELLO) {
		printf("%s: unexpected message type %d\n", __func__, msgtype);
		return -EINVAL;
	}

	min_ver = FIELD_GET(APPLE_RTKIT_MGMT_HELLO_MINVER, msg.msg0);
	max_ver = FIELD_GET(APPLE_RTKIT_MGMT_HELLO_MAXVER, msg.msg0);
	want_ver = min(APPLE_RTKIT_MAX_SUPPORTED_VERSION, max_ver);

	if (min_ver > APPLE_RTKIT_MAX_SUPPORTED_VERSION) {
		printf("%s: firmware min version %d is too new\n",
		       __func__, min_ver);
		return -ENOTSUPP;
	}

	if (max_ver < APPLE_RTKIT_MIN_SUPPORTED_VERSION) {
		printf("%s: firmware max version %d is too old\n",
		       __func__, max_ver);
		return -ENOTSUPP;
	}

	/* EP0_SEND_HELLO */
	msg.msg0 = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_HELLO_REPLY) |
		FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MINVER, want_ver) |
		FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MAXVER, want_ver);
	msg.msg1 = APPLE_RTKIT_EP_MGMT;
	mbox_send(chan, &msg);

wait_epmap:
	/* EP0_WAIT_EPMAP */
	ret = mbox_recv(chan, &msg, 10000);

	endpoint = msg.msg1;
	msgtype = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg.msg0);
	if (endpoint != APPLE_RTKIT_EP_MGMT) {
		printf("%s: unexpected endpoint %d\n", __func__, endpoint);
		return -EINVAL;
	}
	if (msgtype != APPLE_RTKIT_MGMT_EPMAP) {
		printf("%s: unexpected message type %d\n", __func__, msgtype);
		return -EINVAL;
	}

	bitmap = FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BITMAP, msg.msg0);
	base = FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BASE, msg.msg0);
	for (i = 0; i < 32; i++) {
		if (bitmap & (1U << i))
			endpoints[nendpoints++] = base * 32 + i;
	}

	/* EP0_SEND_EPACK */
	reply = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_EPMAP_REPLY);
	if (msg.msg0 & APPLE_RTKIT_MGMT_EPMAP_LAST)
		reply |= APPLE_RTKIT_MGMT_EPMAP_LAST;
	else
		reply |= APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE;
	msg.msg0 = reply;
	msg.msg1 = APPLE_RTKIT_EP_MGMT;
	mbox_send(chan, &msg);

	if (reply & APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE)
		goto wait_epmap;

	for (i = 0; i < nendpoints; i++) {
		/* Don't start the syslog endpoint since we can't handle
		   easily handle its messages in U-Boot. */
		if (endpoints[i] == APPLE_RTKIT_EP_SYSLOG)
			continue;
		/* EP0_SEND_EPSTART */
		msg.msg0 = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_STARTEP) |
			FIELD_PREP(APPLE_RTKIT_MGMT_STARTEP_EP, endpoints[i]) |
			APPLE_RTKIT_MGMT_STARTEP_FLAG;
		msg.msg1 = APPLE_RTKIT_EP_MGMT;
		mbox_send(chan, &msg);
	}

	for (;;) {
		ret = mbox_recv(chan, &msg, 100000);
		if (ret == -ETIMEDOUT)
			break;
		if (ret < 0)
			return ret;

		endpoint = msg.msg1;
		msgtype = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg.msg0);

		if (endpoint == APPLE_RTKIT_EP_MGMT && msgtype == 11)
			break;

		if (endpoint == APPLE_RTKIT_EP_CRASHLOG ||
		    endpoint == APPLE_RTKIT_EP_SYSLOG ||
		    endpoint == APPLE_RTKIT_EP_IOREPORT) {
			u64 size = ((msg.msg0 >> 44) & 0xff) << 12;
			u64 addr = (msg.msg0 & 0xfffffffffff);

			if (msgtype == APPLE_RTKIT_BUFFER_REQUEST && addr != 0)
				continue;

			if (endpoint != APPLE_RTKIT_EP_IOREPORT &&
			    msgtype != APPLE_RTKIT_BUFFER_REQUEST)
				continue;

			size = roundup(size, SZ_16K);
			if (addr == 0 && size > 0 && msgtype == 1) {
				if (apple_rtkit_phys_addr + size >
				    apple_rtkit_phys_start + apple_rtkit_size) {
					printf("%s: out of memory\n", __func__);
					return -ENOMEM;
				}

				addr = apple_rtkit_phys_addr;
				apple_rtkit_phys_addr += size;

				if (sart_map) {
					ret = sart_map(cookie, addr, size);
					if (ret < 0)
						return ret;
				}
			}

			msg.msg0 &= ~0xfffffffffff;
			msg.msg0 |= addr;
			mbox_send(chan, &msg);
			continue;
		}
		if (endpoint != APPLE_RTKIT_EP_MGMT) {
			printf("%s: unexpected endpoint %d\n", __func__, endpoint);
			return -EINVAL;
		}
		if (msgtype != APPLE_RTKIT_MGMT_PWR_STATE_ACK) {
			printf("%s: unexpected message type %d\n", __func__, msgtype);
			return -EINVAL;
		}

		/* EP0_SEND_PWRACK */
		msg.msg0 = 0x00b0000000000020;
		msg.msg1 = APPLE_RTKIT_EP_MGMT;
		mbox_send(chan, &msg);
	}

	return 0;
}
