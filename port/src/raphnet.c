#ifdef PD_ENABLE_RAPHNET

#include <stdio.h>
#include <string.h>
#include <hidapi/hidapi.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "raphnet.h"
#include "mempak.h"

/*
 * Raphnet N64-to-USB adapter transport (USB-HID).
 *
 * Raphnet's gc_n64_usb-v3 family of adapters exposes a "raw" HID interface
 * (the raphnetraw protocol) through which raw N64 SI commands can be issued to
 * the attached controller, including the Controller Pak read (cmd 0x02) and
 * write (cmd 0x03) commands. A 32KB pak is read/written as 128 pages of
 * 256 bytes, each page being eight 32-byte blocks.
 *
 * IMPORTANT: the exact HID report IDs and raphnetraw request opcodes are NOT
 * encoded here. They must be taken from raphnet's gcn64ctl / gc_n64_usb-v3
 * firmware (the raphnetraw protocol headers) and dropped into
 * raphnetRawExchange() / raphnetReadBlock() / raphnetWriteBlock() below. Until
 * that is done the read/write entry points fail safely (no bytes are ever sent
 * to the adapter), so this transport can be built and the device layer
 * exercised without any risk to a physical cartridge.
 */

#define RAPHNET_VID 0x289b

struct raphnet_dev {
	hid_device *handle;
};

static s32 g_RaphnetInited = 0;

s32 raphnetInit(void)
{
	if (g_RaphnetInited) {
		return 0;
	}
	if (hid_init() != 0) {
		sysLogPrintf(LOG_ERROR, "raphnet: hid_init failed");
		return -1;
	}
	g_RaphnetInited = 1;
	return 0;
}

void raphnetShutdown(void)
{
	if (g_RaphnetInited) {
		hid_exit();
		g_RaphnetInited = 0;
	}
}

raphnet_dev *raphnetOpen(void)
{
	static struct raphnet_dev dev;

	if (raphnetInit() != 0) {
		return NULL;
	}

	struct hid_device_info *list = hid_enumerate(RAPHNET_VID, 0x0);
	struct hid_device_info *cur = list;
	hid_device *handle = NULL;

	while (cur) {
		handle = hid_open_path(cur->path);
		if (handle) {
			break;
		}
		cur = cur->next;
	}

	if (list) {
		hid_free_enumeration(list);
	}

	if (!handle) {
		sysLogPrintf(LOG_NOTE, "raphnet: no adapter found (VID %04x)", RAPHNET_VID);
		return NULL;
	}

	dev.handle = handle;
	return &dev;
}

void raphnetClose(raphnet_dev *dev)
{
	if (dev && dev->handle) {
		hid_close(dev->handle);
		dev->handle = NULL;
	}
}

/*
 * Issue one raphnetraw request and read the reply. The report layout and
 * opcodes are firmware-specific and must be filled in before the read/write
 * paths can talk to real hardware. Returns the reply length, or < 0 on error.
 */
static int raphnetRawExchange(raphnet_dev *dev, const u8 *req, int reqlen, u8 *reply, int replymax)
{
	(void)dev;
	(void)req;
	(void)reqlen;
	(void)reply;
	(void)replymax;
	/* TODO(raphnet): implement using the gc_n64_usb-v3 raphnetraw protocol. */
	return -1;
}

s32 raphnetReadPak(raphnet_dev *dev, u8 *buf32k)
{
	if (!dev || !dev->handle) {
		return -1;
	}
	/*
	 * Pseudocode for when raphnetRawExchange() is implemented:
	 *   for (addr = 0; addr < 0x8000; addr += 32)
	 *       issue N64 cmd 0x02 (read mempak) for `addr`, copy 32 bytes back.
	 */
	if (raphnetRawExchange(dev, NULL, 0, NULL, 0) < 0) {
		sysLogPrintf(LOG_WARNING, "raphnet: raw protocol not configured; cannot read pak");
		return -1;
	}
	(void)buf32k;
	return 0;
}

s32 raphnetWritePak(raphnet_dev *dev, const u8 *buf32k)
{
	if (!dev || !dev->handle) {
		return -1;
	}
	/* Writing to a physical cartridge with an unverified protocol could corrupt
	 * it, so refuse until raphnetRawExchange() is implemented. */
	sysLogPrintf(LOG_WARNING, "raphnet: raw protocol not configured; refusing to write pak");
	(void)buf32k;
	return -1;
}

#endif /* PD_ENABLE_RAPHNET */
