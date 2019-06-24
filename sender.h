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
  FILE* fd;
  int max_size;
  int packet_size;
  int port;
  int socket;
} Mandata;


M* parse_args(int,char**);

int exchange(M*);

void finish();
