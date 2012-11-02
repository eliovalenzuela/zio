#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

void print_help(char *name, FILE *f)
{
	fprintf(f, "%s: Use: \"%s [<option> ...] <ziodev>\"\n", name, name);
	fprintf(f, "Available options:\n"
		"   -r          raw socket\n"
		"   -d          datagram socket (default == stream)\n"
		"   -s <n>      channel-set\n"
		"   -c <n>      channel\n"
		"   -i <num>    device id (default: 0)\n"
		"   -v          verbose to stderr\n");
	exit(1);
}

static char buffer[1024*1024]; /* 1MB! */

int main(int argc, char **argv)
{
	int socktype = SOCK_STREAM;
	int c, verbose = 0;
	socklen_t slen;
	int sock, rxed, txed;
	struct sockaddr_zio zaddr;

	memset(&zaddr, 0, sizeof(zaddr));
	zaddr.sa_family = AF_ZIO;
	slen = sizeof(zaddr);

	while ((c = getopt(argc, argv, "rds:c:i:v")) != -1) {
		switch (c) {
		case 'r':
			socktype = SOCK_RAW;
			break;
		case 'd':
			socktype = SOCK_DGRAM;
			break;
		case 's':
			zaddr.cset = atoi(optarg);
			break;
		case 'c':
			zaddr.chan = atoi(optarg);
			break;
		case 'i':
			zaddr.dev_id = atoi(optarg);
			break;
		case 'v':
			verbose++;
			break;
		}
	}
	if (optind != argc - 1)
		print_help(argv[0], stderr);
	strncpy(zaddr.devname, argv[optind], sizeof(zaddr.devname));

	sock = socket(AF_ZIO, socktype, 0);
	if (sock < 0){
		fprintf(stderr, "%s: socket(PF_ZIO): %s\n",
			argv[0], strerror(errno));
		exit(1);
	}
	/* To send we must use connect, not bind */
	if (connect(sock, (struct sockaddr *)&zaddr, slen)) {
		fprintf(stderr, "%s: connect(PF_ZIO): %s\n",
			argv[0], strerror(errno));
		exit(1);
	}
	if (verbose) {
		fprintf(stderr, "%s: connected to \"%.12s\","
			" id 0x%04x, cset 0x%04x, chan 0x%04x\n", argv[0],
			zaddr.devname, zaddr.dev_id, zaddr.cset, zaddr.chan);
	}

	while (1){
		/* stdin is accesses with read() to prevent buffering */
		rxed = read(STDIN_FILENO, buffer, sizeof(buffer));
		if (rxed == 0)
			break;
		if (rxed < 0 && errno == EINTR)
			continue;
		if (rxed < 0) {
			fprintf(stderr, "%s: read(stdin): %s\n", argv[0],
				strerror(errno));
			exit(1);
		}
		if (verbose)
			fprintf(stderr, "%s: sending %i bytes to \"%.12s\","
				" id %0x084x, cset 0x%04x, chan 0x%04x\n",
				argv[0], rxed, zaddr.devname, zaddr.dev_id,
				zaddr.cset, zaddr.chan);
		txed = sendto(sock, buffer, rxed, 0,
			      (struct sockaddr *)&zaddr, sizeof(zaddr));
		if (txed < 0) {
			fprintf(stderr, "%s: sendto(%s-%x.%i.%i): %s\n",
				argv[0], zaddr.devname, zaddr.dev_id,
				zaddr.cset, zaddr.chan, strerror(errno));
		}
		if (txed != rxed)
			fprintf(stderr, "%s: sendto(): sent %i out of %i\n",
				argv[0], txed, rxed);
	}
	close(sock);
	return 0;
}
