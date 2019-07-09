#include <sys/socket.h>
#include <sys/stat.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <getopt.h>
#include <assert.h>
#define DEFAULT_PACKETSIZE 1064;
typedef struct Mandata {
	char* fn;
	FILE* fd;
	char* port;
	int packet_size;
	struct addrinfo* inf;
	void* buf;
} Mandata;


Mandata* parse_args (int, char**);

int exchange (Mandata*);

int finish (Mandata*);

