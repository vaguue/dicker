#include "sender.h"



void main(int argc, char** argv) {
  M* m = parse_args(argc, argv);
  if (m == NULL) {
    exit(0);
  }

  int res = exchange(m);
  if (res < 0) {
    puts("transfer is not succecfull");
  }
  
  finish(m);
}

void finish(M* m) {
  close(m->sock);
  fclose(m->fd);
  free(m->buf);
  free(m->inf);
  free(m);
}

M* parse_args(int argc, char** argv) {
  if (argc < 6) {
    printf("@_@ \nusage: %s -d dest_ip -p port -f filename\n", argv[0]);
    return NULL;
  }

  M* m = malloc(sizeof(M));
  *m = (M){0};
  m->packet_size = DEFAULT_PACKETSIZE;


  
  int c;
  char* ip   = NULL;
  char* port = NULL;

  while ( (c = getopt(argc, argv, "d:p:f:s")) != -1) {
    switch(c) {
      case 'd':
        ip = optarg;
        break;
      case 'p':
        port = optarg;
        break;
      case 'f':
        m->fn = optarg;
        break;
      case 's':
        m->packet_size = atoi (optarg);
        break;
      case '?':
        printf("@_@\n uknown args man\n");
        return NULL;
    }
  }
  
  if (ip == NULL || m->fn == NULL || port == NULL) {
    printf("@_@\n not enough args blin\n");
    return NULL;
  }

  m->inf = malloc(sizeof(struct addrinfo));

  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_CANONNAME;

  int res = getaddrinfo(ip, port, &hints, &m->inf);
  if (res < 0) {
    perror("getaddrinfo");
    return NULL;
  }


  m->buf = malloc(m->packet_size);

  return m;
}


int exchange(M* d) {

  printf("TRASNFERING FILE: %s\n TO ADDR: %s\n", d->fn, d->inf->ai_canonname);
  struct addrinfo* i = d->inf;
  d->sock = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
  int res = connect(d->sock, i->ai_addr, i->ai_addrlen);

  if (res < 0) {
    perror("connect");
    return -1;
  }

  d->fd = fopen(d->fn, "r");

  int a = d->packet_size;

  for (a = fread(d->buf, 1, a, d->fd); a == d->packet_size; a = fread(d->buf, 1, a, d->fd) ) {
    res = send (d->sock, d->buf, d->packet_size, 0);
    if (res < 0) {
      perror("send");
      return -1;
    }
  }
  if (a < 0) {
    perror("fread");
    return -1;
  }
  else if (a > 0) {
    res = send (d->sock, d->buf, a, 0);
    if (res < 0) {
      perror("send");
      return -1;
    }
  }

}
