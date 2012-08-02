#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

#define AF_ZIO			28
#define BUFF_SIZE		4096

int main(int argc, char **argv)
{
	char buffer[BUFF_SIZE], *optvalue, c;
	int file = 0;
	int sockfd, txed = 0, rd = 0, tmp, ret;
	int chan = 0, cset = 1, dev_id = 0, socktype = SOCK_DGRAM;
	struct sockaddr_zio szio;
	struct stat stat;
	size_t ln;

	strncpy(szio.devname, "zzero", ZIO_OBJ_NAME_LEN);

	while ((c = getopt(argc, argv, "c:s:d:t:n:f:")) != -1) {
		switch (c)
		{
		case 'c':
			chan = atoi(optarg);
			break;
		case 's':
			cset = atoi(optarg);
			break;
		case 'd':
			dev_id = atoi(optarg);
			break;
		case 't':
			optvalue = optarg;
			if (strcmp(optvalue, "dgram") == 0)
				socktype = SOCK_DGRAM;
			if (strcmp(optvalue, "stream") == 0)
				socktype = SOCK_STREAM;
			if (strcmp(optvalue, "raw") == 0)
				socktype = SOCK_RAW;
			break;
		case 'n':
			strncpy(szio.devname, optarg, ZIO_OBJ_NAME_LEN);
			break;
		case 'f':
			file = open(optarg, O_RDONLY);
			break;
		}
	}

	printf("Sending to %s, device %d, cset %d, chan %d\n", szio.devname,
							dev_id, cset, chan);

	szio.sa_family = AF_ZIO;
	szio.dev_id = dev_id;
	szio.cset = cset;
	szio.chan = chan;

	sockfd = socket(AF_ZIO, socktype, 0);
	if (sockfd < 0){
		printf("Error creating socket: %s\n", strerror(errno));
		return -1;
	}

	if (connect(sockfd, (struct sockaddr *)&szio, sizeof(szio)) < 0){
		printf("Error connecting socket: %s\n", strerror(errno));
		return -2;
	}


	if (file != 0){
		tmp = fstat(file, &stat);
		if (tmp < 0){
			printf("stat error: %s\n", strerror(errno));
			return -3;
		}
		ln = stat.st_size;
		while (rd < ln){
			tmp = read(file, buffer, BUFF_SIZE);
			rd += tmp;
			while (txed < tmp){
				ret = send(sockfd, buffer + txed,
							BUFF_SIZE - txed, 0);
				if (ret < 0){
					printf("Error sending data: %s\n",
							strerror(errno));
					return -4;
				}
				txed += ret;
			}
			printf("Sent %d bytes\n", txed);
			txed = 0;
		}


	} else {
get_input:	fgets(buffer, BUFF_SIZE, stdin);
		ln = strlen(buffer) - 1;
		if (buffer[ln] == '\n')
			buffer[ln] = '\0';
		printf("Sending %s %d bytes...", buffer, ln);
		txed = send(sockfd, buffer, ln, 0);
		printf("sent %d bytes.\n", txed);
		goto get_input;
	}

	return 0;
}
