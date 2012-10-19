#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

void print_help(char *name, FILE *f)
{
	fprintf(f, "%s: Use: \"%s [<option> ...] <ziodev>\"\n", name, name);
	fprintf(f, "Available options:\n"
		"   -r          raw socket\n"
		"   -d          datagram socket (default == stream)\n"
		"   -s <n>      channel-set (number or \"all\", default 0) \n"
		"   -c <n>      channel (number or \"all\" default 0)\n"
		"   -i <num>    device id (default: 0 default 0)\n"
		"   -v          verbose to stderr\n");
	exit(1);
}

static char buffer[1024*1024]; /* 1MB! */

int main(int argc, char **argv)
{
	int socktype = SOCK_STREAM;
	int c, verbose = 0;
	socklen_t slen;
	int sock, rxed;
	struct sockaddr_zio zaddr, peer;

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
			if (!strcmp(optarg, "all")) {
				zaddr.cset = 0xffff;
			}
			break;
		case 'c':
			zaddr.chan = atoi(optarg);
			if (!strcmp(optarg, "all")) {
				zaddr.cset = 0xffff;
			}
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
	/* FIXME: check what this PF_ZIO_BIND is */
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
		slen = sizeof(peer);
		rxed = recvfrom(sock, buffer, sizeof(buffer), 0,
				(struct sockaddr *)&peer, &slen);
		if (rxed < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "%s: recvfrom(): %s\n", argv[0],
				strerror(errno));
			exit(1);
		}

		if (verbose)
			fprintf(stderr, "%s: received %i bytes from \"%.12s\","
				" id %0x084x, cset 0x%04x, chan 0x%04x\n",
				argv[0], rxed, peer.devname, peer.dev_id,
				peer.cset, peer.chan);
		fwrite(buffer, 1, rxed, stdout);
		fflush(stdout);
	}
	close(sock);
	return 0;
}
