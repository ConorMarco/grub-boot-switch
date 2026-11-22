/* Writes the virtual disk to a file so it can be checked on a PC.
 * Usage: make_image <menu|linux|windows> <output.img> */
#include <stdio.h>
#include <string.h>
#include "../virtualdisk.h"

int main(int argc, char** argv)
{
	BootChoice_t choice;
	uint8_t      buf[VDISK_BLOCK_SIZE];
	FILE*        out;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <menu|linux|windows> <output.img>\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "linux") == 0)
	  choice = BOOT_LINUX;
	else if (strcmp(argv[1], "windows") == 0)
	  choice = BOOT_WINDOWS;
	else if (strcmp(argv[1], "menu") == 0)
	  choice = BOOT_MENU;
	else
	{
		fprintf(stderr, "unknown position: %s\n", argv[1]);
		return 2;
	}

	out = fopen(argv[2], "wb");
	if (out == NULL)
	{
		perror(argv[2]);
		return 1;
	}

	for (uint32_t lba = 0; lba < VDISK_BLOCKS; lba++)
	{
		VirtualDisk_ReadBlock(lba, choice, buf);
		fwrite(buf, 1, sizeof(buf), out);
	}

	return fclose(out) == 0 ? 0 : 1;
}
