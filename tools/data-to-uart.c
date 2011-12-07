/* Trivial program that sends data according to the zio uart protocol */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

static uint64_t get_usecs(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 * 1000LL + tv.tv_usec;
}


int main(int argc, char **argv)
{
	unsigned int i, j;
	uint64_t us1, us2;
	int step_usec = 0;

	if (argc > 1)
		step_usec = atoi(argv[1]);
	if (step_usec)
		fprintf(stderr, "%s: delaying %i usecs between samples\n",
			argv[0], step_usec);

	us1 = get_usecs();
	for (i = 0; ; i++) {
		/*
		 * The protocol is: 0xff 0xff 0x02 0x08 + data
		 * Data is 2 bytes, 8 times, little-endian.
		 * We build 14-bit numbers, counting at different rates
		 */
		putchar(0xff); putchar(0xff); putchar(0x02); putchar(0x08);
		for (j = 0; j < 8; j++) {
			int val = (i >> j) & 0x3fff;
			putchar(val);
			putchar(val >> 8);
		}
		fflush(stdout);
		if (!step_usec)
			continue;
		/* Delay, if needed before writing next samples */
		us2 = get_usecs();
		us1 += step_usec;
		if (us2 < us1)
			usleep(us1 - us2);
	}
}
