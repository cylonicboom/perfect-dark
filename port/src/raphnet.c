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

/* raphnetraw request opcodes (src/requests.h in raphnet/pj64raphnetraw) */
#define RQ_GCN64_RAW_SI_COMMAND 0x80

/* N64 controller-pak (mempak) SI commands */
#define N64_EXPANSION_READ  0x02
#define N64_EXPANSION_WRITE 0x03

/* HID feature-report size used by modern (bio-capable) adapters, plus the
 * leading report-id byte. */
#define RAPHNET_REPORT_SIZE 63
#define RAPHNET_BUF_SIZE    (RAPHNET_REPORT_SIZE + 1)

struct raphnet_dev {
	hid_device *handle;
};

static s32 g_RaphnetInited = 0;

/*
 * N64 controller-pak address CRC5 (verbatim from src/lib/ultra/io/crc.c). The
 * 16-bit pak address is (block << 5) | crc5(block); the adapter forwards it to
 * the controller unchanged, so we must pre-compute the CRC exactly as libultra
 * does.
 */
static u8 raphnetAddrCrc(u16 inaddr)
{
	u32 crc = 0;
	u32 mask;
	u32 addr = inaddr;

	for (mask = 0x400; mask != 0; mask >>= 1) {
		crc *= 2;
		if (addr & mask) {
			crc = (crc & 0x20) ? (crc ^ 20) : (crc + 1);
		} else if (crc & 0x20) {
			crc ^= 21;
		}
	}

	for (s32 i = 0; i < 5; i++) {
		crc <<= 1;
		if (crc & 0x20) {
			crc ^= 21;
		}
	}

	return crc & 0x1f;
}

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
 * Send one raphnetraw command as a HID feature report and poll for the reply.
 * Mirrors gcn64_exchange(): report-id byte 0, then the command bytes; the reply
 * is ready once the device echoes the command byte back. `reply` receives the
 * payload with the report-id byte stripped (so reply[0] is the command echo).
 * Returns the reply length, or < 0 on error.
 */
static int gcn64Exchange(hid_device *handle, const u8 *cmd, int cmdlen, u8 *reply, int replymax)
{
	u8 buf[RAPHNET_BUF_SIZE];

	if (cmdlen + 1 > (int)sizeof(buf)) {
		return -1;
	}

	memset(buf, 0, sizeof(buf));
	buf[0] = 0; // report id (device exposes a single report)
	memcpy(buf + 1, cmd, cmdlen);

	if (hid_send_feature_report(handle, buf, sizeof(buf)) < 0) {
		return -1;
	}

	/*
	 * Poll until the device returns a non-empty result. While the command is
	 * still being processed the adapter returns an empty report (<= 1 byte, the
	 * report id); a real reply has res_len = bytes - 1 > 0. (Matches
	 * gcn64_exchange/gcn64_poll_result in raphnet/pj64raphnetraw.)
	 */
	for (s32 attempt = 0; attempt < 1000; attempt++) {
		int r, reslen;
		memset(buf, 0, sizeof(buf));
		buf[0] = 0;
		r = hid_get_feature_report(handle, buf, sizeof(buf));
		if (r < 0) {
			return -1;
		}
		reslen = r - 1;
		if (reslen > 0) {
			if (reslen > replymax) {
				reslen = replymax;
			}
			memcpy(reply, buf + 1, reslen);
			return reslen;
		}
	}

	return -1;
}

// Issue a raw N64 SI command on `channel`. Returns the number of reply bytes.
static int raphnetRawSiCommand(hid_device *handle, u8 channel, const u8 *tx, u8 txlen, u8 *rx, int maxrx)
{
	u8 cmd[3 + 64];
	u8 rep[3 + 64];

	if (txlen > 64) {
		return -1;
	}

	cmd[0] = RQ_GCN64_RAW_SI_COMMAND;
	cmd[1] = channel;
	cmd[2] = txlen;
	memcpy(cmd + 3, tx, txlen);

	int n = gcn64Exchange(handle, cmd, 3 + txlen, rep, sizeof(rep));
	if (n < 3) {
		return -1;
	}

	// rep: [cmd echo][channel][rx_len][rx data...]
	int rxlen = rep[2];
	if (rxlen > maxrx) {
		rxlen = maxrx;
	}
	if (rxlen > 0 && rx) {
		memcpy(rx, rep + 3, rxlen);
	}
	return rxlen;
}

// Build the 16-bit pak address for `block` (32-byte unit), CRC5 in the low bits.
static u16 raphnetPakAddr(u16 block)
{
	return (u16)((block << 5) | raphnetAddrCrc(block));
}

s32 raphnetReadPak(raphnet_dev *dev, u8 *buf32k)
{
	if (!dev || !dev->handle) {
		return -1;
	}

	for (u16 block = 0; block < MEMPAK_SIZE / 32; block++) {
		u16 addr = raphnetPakAddr(block);
		u8 tx[3] = { N64_EXPANSION_READ, (u8)(addr >> 8), (u8)(addr & 0xff) };
		u8 rx[33];

		int n = raphnetRawSiCommand(dev->handle, 0, tx, sizeof(tx), rx, sizeof(rx));
		if (n < 32) {
			sysLogPrintf(LOG_WARNING, "raphnet: read failed at block %u (got %d)", block, n);
			return -1;
		}

		memcpy(buf32k + block * 32, rx, 32);
	}

	return 0;
}

s32 raphnetWritePak(raphnet_dev *dev, const u8 *buf32k)
{
	if (!dev || !dev->handle) {
		return -1;
	}

	for (u16 block = 0; block < MEMPAK_SIZE / 32; block++) {
		u16 addr = raphnetPakAddr(block);
		u8 tx[3 + 32];
		u8 rx[4];

		tx[0] = N64_EXPANSION_WRITE;
		tx[1] = (u8)(addr >> 8);
		tx[2] = (u8)(addr & 0xff);
		memcpy(tx + 3, buf32k + block * 32, 32);

		// The controller computes and returns the data CRC; one reply byte.
		int n = raphnetRawSiCommand(dev->handle, 0, tx, sizeof(tx), rx, sizeof(rx));
		if (n < 1) {
			sysLogPrintf(LOG_WARNING, "raphnet: write failed at block %u (got %d)", block, n);
			return -1;
		}
	}

	return 0;
}

#endif /* PD_ENABLE_RAPHNET */
