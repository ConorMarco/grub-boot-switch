#include <string.h>
#include "virtualdisk.h"

/*
 * Layout, in 512-byte blocks:
 *   0      MBR, one partition (type 0x01, FAT12) from block 1 to the end
 *   1      FAT12 boot sector
 *   2-3    FAT copy 1
 *   4-5    FAT copy 2
 *   6      root folder (16 entries)
 *   7-     data area, 510 clusters of 4 blocks, all free and zero
 */
#define PART_START      1u
#define PART_BLOCKS     (VDISK_BLOCKS - PART_START)
#define FAT_BLOCKS      2u
#define FAT1_LBA        (PART_START + 1u)
#define FAT2_LBA        (FAT1_LBA + FAT_BLOCKS)
#define ROOT_LBA        (FAT2_LBA + FAT_BLOCKS)

#define VOLUME_ID       0xB0075317UL   /* shown as B007-5317 */

/* 2026-11-17, in FAT date format: ((year - 1980) << 9) | (month << 5) | day */
#define FAT_DATE        (((2026u - 1980u) << 9) | (11u << 5) | 17u)

static void put16(uint8_t* p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static void BuildMBR(uint8_t* buf)
{
	uint8_t* entry = &buf[446];

	put32(&buf[440], VOLUME_ID);      /* disk signature */
	entry[0] = 0x00;                  /* not active */
	entry[1] = 0x00; entry[2] = 0x02; entry[3] = 0x00;   /* CHS start: head 0, sector 2 */
	entry[4] = 0x01;                  /* FAT12 */
	entry[5] = 0xFE; entry[6] = 0xFF; entry[7] = 0xFF;   /* CHS end: use LBA */
	put32(&entry[8], PART_START);
	put32(&entry[12], PART_BLOCKS);
	buf[510] = 0x55;
	buf[511] = 0xAA;
}

static void BuildBootSector(uint8_t* buf)
{
	buf[0] = 0xEB; buf[1] = 0x3C; buf[2] = 0x90;
	memcpy(&buf[3], "MSWIN4.1", 8);
	put16(&buf[11], VDISK_BLOCK_SIZE);  /* bytes per sector */
	buf[13] = 4;                        /* sectors per cluster */
	put16(&buf[14], 1);                 /* reserved sectors */
	buf[16] = 2;                        /* number of FATs */
	put16(&buf[17], 16);                /* root folder entries */
	put16(&buf[19], PART_BLOCKS);       /* total sectors */
	buf[21] = 0xF8;                     /* media: fixed disk */
	put16(&buf[22], FAT_BLOCKS);        /* sectors per FAT */
	put16(&buf[24], 32);                /* sectors per track */
	put16(&buf[26], 2);                 /* heads */
	put32(&buf[28], PART_START);        /* hidden sectors */
	put32(&buf[32], 0);                 /* total sectors (32-bit), unused */
	buf[36] = 0x80;                     /* drive number */
	buf[38] = 0x29;                     /* extended boot signature */
	put32(&buf[39], VOLUME_ID);
	memcpy(&buf[43], "BOOTSWITCH ", 11);
	memcpy(&buf[54], "FAT12   ", 8);
	buf[510] = 0x55;
	buf[511] = 0xAA;
}

static void BuildRootFolder(BootChoice_t choice, uint8_t* buf)
{
	static const char* const names[] =
	{
		[BOOT_MENU]    = "MENU    SW ",
		[BOOT_LINUX]   = "LINUX   SW ",
		[BOOT_WINDOWS] = "WINDOWS SW ",
	};

	/* Entry 0: volume label */
	memcpy(&buf[0], "BOOTSWITCH ", 11);
	buf[11] = 0x08;
	put16(&buf[24], FAT_DATE);

	/* Entry 1: empty read-only file named after the switch position */
	memcpy(&buf[32], names[choice], 11);
	buf[32 + 11] = 0x01;
	put16(&buf[32 + 16], FAT_DATE);     /* created */
	put16(&buf[32 + 18], FAT_DATE);     /* accessed */
	put16(&buf[32 + 24], FAT_DATE);     /* modified */
	/* First cluster 0 and size 0: the file has no data. */
}

void VirtualDisk_ReadBlock(uint32_t lba, BootChoice_t choice, uint8_t* buf)
{
	memset(buf, 0, VDISK_BLOCK_SIZE);

	if (choice > BOOT_WINDOWS)
	  choice = BOOT_MENU;

	if (lba == 0)
	{
		BuildMBR(buf);
	}
	else if (lba == PART_START)
	{
		BuildBootSector(buf);
	}
	else if ((lba == FAT1_LBA) || (lba == FAT2_LBA))
	{
		/* Clusters 0 and 1 are reserved; every other cluster is free. */
		buf[0] = 0xF8;
		buf[1] = 0xFF;
		buf[2] = 0xFF;
	}
	else if (lba == ROOT_LBA)
	{
		BuildRootFolder(choice, buf);
	}
}
