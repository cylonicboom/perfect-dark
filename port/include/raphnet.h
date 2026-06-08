#ifndef _IN_PORT_RAPHNET_H
#define _IN_PORT_RAPHNET_H

#include <PR/ultratypes.h>

/*
 * Transport for reading/writing a physical N64 Controller Pak through a
 * Raphnet N64-to-USB adapter over USB-HID. The exact wire protocol (HID report
 * IDs and the raphnetraw opcodes) is isolated inside raphnet.c.
 *
 * Only compiled when PD_ENABLE_RAPHNET is defined.
 */

#ifdef PD_ENABLE_RAPHNET

typedef struct raphnet_dev raphnet_dev;

/* Initialise the HID backend. Returns 0 on success. */
s32 raphnetInit(void);

/* Open the first connected Raphnet adapter, or NULL if none is present. */
raphnet_dev *raphnetOpen(void);

/* Read the full 32KB Controller Pak image into `buf32k`. Returns 0 on success. */
s32 raphnetReadPak(raphnet_dev *dev, u8 *buf32k);

/* Write the full 32KB Controller Pak image from `buf32k`. Returns 0 on success. */
s32 raphnetWritePak(raphnet_dev *dev, const u8 *buf32k);

void raphnetClose(raphnet_dev *dev);
void raphnetShutdown(void);

#endif /* PD_ENABLE_RAPHNET */

#endif
