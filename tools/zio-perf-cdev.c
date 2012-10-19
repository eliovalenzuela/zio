#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>

#include <linux/zio-user.h>

/*
 * This should run with a software trigger (timer, hrt, whatever) in order
 * to have a meaningful timestamp in the control
 */
int main(int argc, char **argv)
{
	struct zio_control ctrl;
	int fd;
	struct timeval tv;
	unsigned long long tdiff;
	int i;

	if (argc != 2) {
		fprintf(stderr, "%s: Wrong number of arguments\n"
			"Use: \"%s <ctrl-file>\"\n",
			argv[0], argv[0]);
		exit(1);
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], argv[1],
			strerror(errno));
		exit(1);
	}
	while (1) {
		i = read(fd, &ctrl, sizeof(ctrl));
		gettimeofday(&tv, NULL);
		if (i < 0) {
			fprintf(stderr, "%s: read(%s): %s\n", argv[0], argv[1],
				strerror(errno));
			exit(1);
		}
		if (i != sizeof(ctrl)) {
			fprintf(stderr, "%s: read(%s): short read\n", argv[0],
				argv[1]);
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
