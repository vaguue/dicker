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

#define M Mandata
#define DEFAULT_PACKETSIZE 1064

typedef struct Mandata {

  char* fn;
  FILE* fd;
  int max_size;
  int packet_size;
  int port;
  int sock;
  struct stat st;
  struct addrinfo* inf;
  void* buf;
  
} Mandata;


M* parse_args(int,char**);

int exchange(M*);

void finish(M*);
