#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

int main(int argc, char **argv)
{
	struct zio_control ctrl;
	struct timeval tv;
	unsigned long long tdiff;
	int i, sock;
	int socktype = SOCK_RAW;
	struct sockaddr_zio zaddr;

	if (argc != 5) {
		fprintf(stderr, "%s: Wrong number of arguments\n"
			"Use: \"%s <devname> <devid> <cset> <cham>\"\n",
			argv[0], argv[0]);
		exit(1);
	}

	memset(&zaddr, 0, sizeof(zaddr));
	zaddr.sa_family = AF_ZIO;
	strncpy(zaddr.devname, argv[1], sizeof(zaddr.devname));
	zaddr.dev_id = atoi(argv[2]);
	zaddr.cset = atoi(argv[3]);
	zaddr.chan = atoi(argv[4]);

	sock = socket(AF_ZIO, socktype, 0);
	if (sock < 0){
		fprintf(stderr, "%s: socket(PF_ZIO): %s\n",
			argv[0], strerror(errno));
		exit(1);
	}
	if (connect(sock, (struct sockaddr *)&zaddr, sizeof(zaddr))) {
		fprintf(stderr, "%s: connect(PF_ZIO): %s\n",
			argv[0], strerror(errno));
		exit(1);
	}
	while (1) {
		i = recv(sock, &ctrl, sizeof(ctrl), MSG_TRUNC);
		gettimeofday(&tv, NULL);
		if (i < 0) {
			 fprintf(stderr, "%s: recv(): %s\n", argv[0],
				 strerror(errno));
			exit(1);
		}
		if (i < sizeof(ctrl)) {
			fprintf(stderr, "%s: recv(): short read\n", argv[0]);
			exit(1);
		}
		/* FIXME: should check major and minor */

		tdiff = tv.tv_sec - ctrl.tstamp.secs;
		tdiff *= 1000 * 1000 * 1000;
		tdiff += tv.tv_usec * 1000 - ctrl.tstamp.ticks;
		printf("%9lli\n", tdiff);
	}
	exit(0);
}
