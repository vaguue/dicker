#include <sys/socket.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
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
  int packet_size;
  int sock;
  struct addrinfo* inf;
  void* buf;
  bool snd;
  
} Mandata;


M* parse_args(int,char**);

int exchange(M*);

void finish(int, M*);
