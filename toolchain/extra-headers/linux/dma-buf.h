/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace dma-buf interface (include/uapi/linux/dma-buf.h), added in Linux 4.6.
 * Our cross toolchain ships Linux 4.4 headers, matching the TV's kernel, so this file is
 * copied into the sysroot (see NOTES.md). WPE WebKit's GBM code needs it to compile; that
 * code only runs for GPU buffers, which the engine never creates (SHM rendering).
 */
#ifndef _DMA_BUF_UAPI_H_
#define _DMA_BUF_UAPI_H_

#include <linux/types.h>
#include <linux/ioctl.h>

struct dma_buf_sync {
	__u64 flags;
};

#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK \
	(DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)

#define DMA_BUF_NAME_LEN	32

#define DMA_BUF_BASE		'b'
#define DMA_BUF_IOCTL_SYNC	_IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

#define DMA_BUF_SET_NAME	_IOW(DMA_BUF_BASE, 1, const char *)
#define DMA_BUF_SET_NAME_A	_IOW(DMA_BUF_BASE, 1, __u32)
#define DMA_BUF_SET_NAME_B	_IOW(DMA_BUF_BASE, 1, __u64)

#endif
