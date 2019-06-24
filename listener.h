#include <sys/socket.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>

#define M Mandata

typedef struct Mandata {
	char* fn;
	FILE* fd;//File descriptor
	int max_size;
	int pocket_size;
	int socket;
	int port;
} Mandata;


M* parse_args(int, char*);//gets args from command line

int exchange (M*);

void finish();//at exit)0))




