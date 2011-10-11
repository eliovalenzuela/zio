/*
 * Trivial utility that reports data from ZIO input channels
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "zio.h"
#include "zio-buffer.h"

#define FD_C 0
#define FD_D 1

unsigned char buf[1024*1024];

int main(int argc, char **argv)
{
	FILE *f;
	char *outfname;
	int fd[2];
	int i, j;

	if (argc !=3) {
		fprintf(stderr, "%s: use \"%s <ctrl-file> <data-file>\"\n",
			argv[0], argv[0]);
		exit(1);
	}

	for (i = 0; i < 2; i++) {
		fd[i] = open(argv[i + 1], O_RDONLY);
		if (fd[i] < 0) {
			fprintf(stderr, "%s: %s: %s\n", argv[0], argv[i + 1],
				strerror(errno));
			exit(1);
		}
	}
	/* the data channel is non-blocking */
	fcntl(fd[FD_D], F_SETFL, fcntl(fd[FD_D], F_GETFL) | O_NONBLOCK);

	/* always log data we read to some filename */
	outfname = getenv("ZIO_DUMP_TO");
	if (!outfname)
		outfname = "/dev/null";
	f = fopen(outfname, "w");
	if (!f) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], outfname,
			strerror(errno));
		exit(1);
	}
	/* ensure proper strace information and output to pipes */
	setlinebuf(stdout);
	setbuf(f, NULL);

	/* now read control and data, forever */
	while (1) {
		zio_control_t ctrl;

		/* This is a blocking read to the control file */
		i = read(fd[FD_C], &ctrl, sizeof(ctrl));
		switch(i) {
		case -1:
			fprintf(stderr, "%s: %s: read(): %s\n",
				argv[0], argv[1 + FD_C], strerror(errno));
			exit(1);
		case 0:
			fprintf(stderr, "%s: %s: unexpected EOF\n",
				argv[0], argv[1 + FD_C], strerror(errno));
			exit(1);
		default:
			fprintf(stderr, "%s: ctrl: read %i bytes (exp %i)\n",
				argv[0], i, sizeof(ctrl));
			/* continue anyways */
		case sizeof(ctrl):
			break; /* ok */
		}
		printf("Ctrl: n %i, size %i, bits %i, flags %08x\n",
			ctrl.ctrl.nsamples,
			ctrl.ctrl.ssize,
			ctrl.ctrl.sbits,
			ctrl.ctrl.flags);
		printf("Ctrl: stamp %li.%09li (%lli)\n",
			ctrl.ctrl.tstamp.tv_sec,
			ctrl.ctrl.tstamp.tv_nsec,
			ctrl.ctrl.tstamp_extra);
		/* FIXME: some control information is missing */

		i = read(fd[FD_D], buf, sizeof(buf));
		if (i < 0) {
			fprintf(stderr, "%s: %s: read(): %s\n",
				argv[0], argv[1 + FD_D], strerror(errno));
			continue; /* next ctrl, let's see... */
		}
		if (!i) {
			fprintf(stderr, "%s: %s: unexpected EOF\n",
				argv[0], argv[1 + FD_D], strerror(errno));
			continue;
		}
		if (i != ctrl.ctrl.nsamples * ctrl.ctrl.ssize) {
			if (i == sizeof(buf)) {
				fprintf(stderr, "%s: buffer too small\n",
					argv[0]);
				/* FIXME: empty the data channel */
			} else {
				fprintf(stderr, "%s: ctrl: read %i bytes "
					"(exp %i)\n", argv[0], i,
					ctrl.ctrl.nsamples * ctrl.ctrl.ssize);
			}
			/* continue anyways */
		}
		fwrite(buf, 1, i, f);

		/* report data to stdout */
		for (j = 0; j < i; j++) {
			if (!(j & 0xf))
				printf("Data:");
			printf(" %02x", buf[j]);
			if ((j & 0xf) == 0xf || j == i - 1)
			putchar('\n');
		}
	putchar('\n');
	}
}
