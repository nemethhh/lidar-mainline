// SPDX-License-Identifier: GPL-2.0
/* Minimal 32-bit MMIO peek/poke for bring-up: devmem ADDR [VALUE] */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s ADDR [VALUE]\n", argv[0]);
		return 2;
	}
	uint64_t addr = strtoull(argv[1], NULL, 0);
	long pg = sysconf(_SC_PAGESIZE);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	volatile uint8_t *map = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED,
				     fd, addr & ~(uint64_t)(pg - 1));
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	volatile uint32_t *reg = (volatile uint32_t *)(map + (addr & (pg - 1)));
	if (argc > 2)
		*reg = (uint32_t)strtoul(argv[2], NULL, 0);
	printf("0x%08x\n", *reg);
	return 0;
}
