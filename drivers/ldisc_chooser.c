/*
 * ldisc_chooser: trivial program to activate a line discipline
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
	int fd, i;

	if (argc != 3) {
		fprintf(stderr, "%s: Use \"%s <dev> <ldisc-nr>\"\n",
			argv[0], argv[0]);
		exit(1);
	}
	if( (fd = open(argv[1], O_RDWR  | O_NOCTTY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], argv[1],
			strerror(errno));
		exit(1);
	}
	if (sscanf(argv[2], "%i%s", &i, argv[2]) != 1) {
		fprintf(stderr, "%s: not a number: %s\n", argv[0], argv[2]);
		exit(1);
	}
	if (ioctl(fd, TIOCSETD, &i)<0) {
		fprintf(stderr, "%s: %s: setldisc: %s\n", argv[0], argv[1],
			strerror(errno));
		exit(1);
	}
	/* wait to be killed */
	while (1)
		sleep(INT_MAX);
	return 0;
}
