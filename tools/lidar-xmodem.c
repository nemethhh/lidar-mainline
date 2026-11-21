// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-xmodem: send a file with XMODEM-CRC (128 byte blocks), as the
 * EFR32's Gecko bootloader expects after its "1. upload gbl" command.
 *
 * Usage: lidar-xmodem <tty> <file>
 * The tty must already be configured (raw, 115200) and the receiver must be
 * waiting: it sends 'C' to start. Exit status 0 once the receiver
 * acknowledged the end of transmission.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SOH	0x01
#define EOT	0x04
#define ACK	0x06
#define NAK	0x15
#define CAN	0x18
#define CRC_C	'C'
#define BLOCK	128
#define RETRIES	10

static int getbyte(int fd, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	unsigned char c;

	if (poll(&p, 1, timeout_ms) <= 0 || read(fd, &c, 1) != 1)
		return -1;
	return c;
}

static uint16_t crc16(const unsigned char *p, int n)
{
	uint16_t crc = 0;

	while (n--) {
		crc ^= (uint16_t)*p++ << 8;
		for (int i = 0; i < 8; i++)
			crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc;
}

static int write_all(int fd, const unsigned char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= w;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char frame[3 + BLOCK + 2];
	unsigned char num = 1;
	int tty, in, c;

	if (argc != 3) {
		fprintf(stderr, "usage: lidar-xmodem <tty> <file>\n");
		return 2;
	}
	tty = open(argv[1], O_RDWR | O_NOCTTY);
	in = open(argv[2], O_RDONLY);
	if (tty < 0 || in < 0) {
		perror("lidar-xmodem: open");
		return 1;
	}

	/* the receiver asks for CRC mode with 'C'; skip anything else (its
	 * banner), giving up after 60 s without one */
	for (int timeouts = 0; (c = getbyte(tty, 1000)) != CRC_C;)
		if (c < 0 && ++timeouts > 60) {
			fprintf(stderr, "lidar-xmodem: receiver not ready\n");
			return 1;
		}

	for (;;) {
		ssize_t n = read(in, frame + 3, BLOCK);
		int tries;

		if (n < 0) {
			perror("lidar-xmodem: read");
			return 1;
		}
		if (n == 0)
			break;
		memset(frame + 3 + n, 0x1a, BLOCK - n);	/* CP/M EOF padding */
		frame[0] = SOH;
		frame[1] = num;
		frame[2] = (unsigned char)~num;
		uint16_t crc = crc16(frame + 3, BLOCK);
		frame[3 + BLOCK] = crc >> 8;
		frame[4 + BLOCK] = crc & 0xff;

		for (tries = 0; tries < RETRIES; tries++) {
			if (write_all(tty, frame, sizeof(frame)) < 0) {
				perror("lidar-xmodem: write");
				return 1;
			}
			/* a late extra 'C' is not an answer to the block */
			do
				c = getbyte(tty, 3000);
			while (c == CRC_C);
			if (c == ACK)
				break;
			if (c == CAN) {
				fprintf(stderr, "lidar-xmodem: cancelled by receiver\n");
				return 1;
			}
		}
		if (tries == RETRIES) {
			fprintf(stderr, "lidar-xmodem: block %u not acknowledged\n", num);
			return 1;
		}
		num++;
	}

	for (int tries = 0; tries < RETRIES; tries++) {
		unsigned char eot = EOT;

		if (write_all(tty, &eot, 1) < 0) {
			perror("lidar-xmodem: write");
			return 1;
		}
		if (getbyte(tty, 3000) == ACK)
			return 0;
	}
	fprintf(stderr, "lidar-xmodem: end of transmission not acknowledged\n");
	return 1;
}
