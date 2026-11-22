/*
 * A read-only 1 MiB disk built on the fly. It has an MBR with one FAT12 partition
 * (volume ID B007-5317, label BOOTSWITCH) whose root folder holds a single empty
 * file named after the boot switch position: LINUX.SW, WINDOWS.SW or MENU.SW.
 *
 * Plain C with no AVR dependencies, so the same code builds on a PC for testing.
 */
#ifndef VIRTUALDISK_H
#define VIRTUALDISK_H

#include <stdint.h>

#define VDISK_BLOCK_SIZE   512u
#define VDISK_BLOCKS       2048u

typedef enum
{
	BOOT_MENU    = 0,
	BOOT_LINUX   = 1,
	BOOT_WINDOWS = 2,
} BootChoice_t;

/* Fills buf (VDISK_BLOCK_SIZE bytes) with the contents of block lba. */
void VirtualDisk_ReadBlock(uint32_t lba, BootChoice_t choice, uint8_t* buf);

#endif
