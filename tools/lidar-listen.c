// SPDX-License-Identifier: GPL-2.0
/*
 * TCP listener tied to one network interface: lidar-listen IFACE PORT CMD...
 *
 * The listening socket is bound to IFACE with SO_BINDTODEVICE, so the kernel
 * only hands it connections that arrive on that interface, whatever address
 * they are sent to. CMD runs once per connection with the connection as
 * stdin and stdout, as under inetd.
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 4 || strlen(argv[1]) >= IFNAMSIZ) {
		fprintf(stderr, "usage: %s IFACE PORT CMD...\n", argv[0]);
		return 2;
	}
	int port = atoi(argv[2]);

	if (port < 1 || port > 65535) {
		fprintf(stderr, "usage: %s IFACE PORT CMD...\n", argv[0]);
		return 2;
	}
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	int one = 1;
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	/* No fallback: without the interface there is no listener at all. */
	if (s < 0 ||
	    setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, argv[1], strlen(argv[1]) + 1) ||
	    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
	    bind(s, (struct sockaddr *)&sa, sizeof(sa)) || listen(s, 4)) {
		perror(argv[1]);
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);	/* children are reaped by the kernel */
	for (;;) {
		int c = accept(s, NULL, NULL);

		if (c < 0) {
			sleep(1);	/* EMFILE and friends: do not spin */
			continue;
		}
		if (fork() == 0) {
			dup2(c, 0);
			dup2(c, 1);
			if (c > 1)
				close(c);
			signal(SIGCHLD, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
			execvp(argv[3], argv + 3);
			_exit(127);
		}
		close(c);
	}
}
