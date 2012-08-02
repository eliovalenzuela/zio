#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

#define AF_ZIO			28

#define PF_ZIO_BIND		1
#define BUFF_SIZE_DGRAM_STREAM	32
#define BUFF_SIZE_RAW		1056

int dumpstruct(void *ptr, int size)
{
	int ret = 0, i;
	unsigned char *p = ptr;

	for (i = 0; i < size; ) {
		printf("%02x", p[i]);
		i++;
		printf(i & 3 ? " " : i & 0xf ? "  " : "\n");
	}
	if (i & 0xf)
		printf("\n");
	return ret;
}

void print_help(void)
{
	printf("Not enough arguments!\n");
	printf("Example usage: pf-zio-receive -t dgram -s 0 -c 2\n\n");
	printf("Available options:\n");
	printf("\t-t  : socket type. Takes a parameter among \"dgram\", \"stream\" and \"raw\"\n");
	printf("\t-s  : channel set. Takes a number as argument.\n");
	printf("\t-c  : channel. Takes a number as argument\n");
	printf("\t-d  : device id. Takes a number as arguments, default to 0\n");
	printf("\t-n  : device name. Takes a string identifyng the zio device\n");
	printf("\t-t  : bind to multiple channels. Takes a parameter among \"cset\" and \"dev\"\n");
	printf("\t-v  : verbose, prints some additional info. Use only once.\n");
}

int main(int argc, char **argv)
{
	int socktype = 0, cset = 0, chan = 0, dev_id = 0;
	int c, verbose = 0, flag = 0;
	socklen_t sziolen;
	int sockfd, rxed;
	struct sockaddr_zio szio, szfrom;
	char *optvalue = NULL, *buffer;

	if (argc < 5){
		print_help();
		return 0;
	}

	while ((c = getopt(argc, argv, "t:s:c:d:n:b")) != -1){
		switch (c)
		{
		case 't':
			optvalue = optarg;
			printf("%s\n", optvalue);
			if (strcmp(optvalue, "dgram") == 0){
				socktype = SOCK_DGRAM;
				buffer = malloc(BUFF_SIZE_DGRAM_STREAM);
			}
			if (strcmp(optvalue, "stream") == 0){
				socktype = SOCK_STREAM;
				buffer = malloc(BUFF_SIZE_DGRAM_STREAM);
			}
			if (strcmp(optvalue, "raw") == 0){
				socktype = SOCK_RAW;
				buffer = malloc(BUFF_SIZE_RAW);
			}
			break;
		case 's':
			optvalue = optarg;
			cset = atoi(optvalue);
			break;
		case 'c':
			optvalue = optarg;
			chan = atoi(optvalue);
			break;
		case 'd':
			optvalue = optarg;
			dev_id = atoi(optvalue);
			break;
		case 'n':
			optvalue = optarg;
			strncpy(szio.devname, optvalue, ZIO_OBJ_NAME_LEN);
			break;
		case 'b':
			optvalue = optarg;
			if (strcmp(optvalue, "cset") == 0)
				chan = 0xFFFF;
			if (strcmp(optvalue, "dev") == 0)
				cset = 0xFFFF;
			flag |= PF_ZIO_BIND;
			break;
		case 'v':
			verbose++;
			break;
		}
	}

	szio.sa_family = AF_ZIO;
	szio.dev_id = dev_id;
	szio.cset = cset;
	szio.chan = chan;

	sockfd = socket(AF_ZIO, socktype, 0);
	if (sockfd < 0){
		printf("Error creating socket: %s\n", strerror(errno));
		return -1;
	}

	if(flag & PF_ZIO_BIND){
		if (bind(sockfd, (struct sockaddr *)&szio, sizeof(szio)) < 0){
			printf("Error binding socket: %s\n", strerror(errno));
			return -2;
		}
	} else {
		if (connect(sockfd, (struct sockaddr *)&szio, sizeof(szio))){
			printf("Error connectin socket: %s\n", strerror(errno));
			return -3;
		}
	}

	sziolen = sizeof(szfrom);

	while (1){
		rxed = recvfrom(sockfd, buffer, sizeof(buffer), 0,
				(struct sockaddr *)&szfrom, &sziolen);
		if (verbose)
			printf("Received %d bytes from device %s id %d channel %d cset %d\n",
				rxed, szfrom.devname, szfrom.dev_id, szfrom.chan, szfrom.cset);
		if (rxed < 0){
			if (errno == EAGAIN){
				printf("EAGAIN\n");
				continue;
			}
			printf("%s\n", strerror(errno));
			break;
		}
		dumpstruct(buffer, rxed);
	}

	close(sockfd);
	return 0;
}
