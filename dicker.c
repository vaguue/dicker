#include "dicker.h"



void main(int argc, char** argv) {
  M* m = parse_args(argc, argv);
  if (m == NULL) {
    exit(0);
  }

  int res = exchange(m);
  if (res < 0) {
    puts("transfer is not succecfull");
  }
  
  finish(res, m);
}

void finish(int res, M* m) {
  puts("@_@\n FINISHED TRANSFERING FILE MANE");
  close(m->sock);
  if (res != -2 ) fclose(m->fd);
  free(m->buf);
  freeaddrinfo(m->inf);
  free(m);
}

M* parse_args(int argc, char** argv) {
  if (argc < 4) {
    printf("@_@ \nusage: %s -d dest_ip -p port -f filename\n", argv[0]);
    return NULL;
  }

  M* m = malloc(sizeof(M));
  *m = (M){0};
  m->packet_size = DEFAULT_PACKETSIZE;
  m->snd = false;


  
  int c;
  char* ip   = NULL;
  char* port = NULL;

  while ( (c = getopt(argc, argv, "d:p:f:s")) != -1) {
    switch(c) {
      case 'd':
        ip = optarg;
        m->snd = true;
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
  
  if ( (m->snd == true && ip == NULL) || m->fn == NULL || port == NULL) {
    printf("@_@\n not enough args blin\n");
    free(m);
    return NULL;
  }

  m->inf = malloc(sizeof(struct addrinfo));
  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int res;

  if (m->snd) {

    hints.ai_flags = AI_CANONNAME;

    res = getaddrinfo(ip, port, &hints, &m->inf);

  }
  else {

    hints.ai_flags =
      AI_PASSIVE;//Автоопределение IP слушателя

    res = getaddrinfo(NULL, port, &hints,
                          &m->inf); //В inf запишет всю нужную для сокета инфу
  }

  if (res < 0) {
    free(m->inf);
    free(m);
    perror("getaddrinfo");
    return NULL;
  }


  m->buf = malloc(m->packet_size);

  return m;
}


int exchange(M* d) {

  if (d->snd) goto _send;
  else goto _recv;

_send:
  do {
  printf("TRASNFERING FILE: %s\n TO ADDR: %s\n", d->fn, d->inf->ai_canonname);
  struct addrinfo* i = d->inf;
  d->sock = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
  int res = connect(d->sock, i->ai_addr, i->ai_addrlen);

  if (res < 0) {
    perror("connect");
    return -2;
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
  goto _exit;
  } while (0);
_recv:
  do {
  //нужно создать сокет
  int sock = socket(d->inf->ai_family, d->inf->ai_socktype,
                    d->inf->ai_protocol);
  if (sock == -1) {
    puts("failed to create socket");
    return -1;
  }
  puts("sock created");
  //теперь скажем на каком порте будем слушать

  int res = bind (sock, d->inf->ai_addr, d->inf->ai_addrlen);
  if (res == -1) {
    puts("failed to bind socket");
    return -1;
  }
  puts("sock binded");
  res = listen(sock, 1);
  if (res == -1) {
    puts("Cannot hear you");
    return -1;
  }
  puts("sock listened");
  int new_sock = accept(sock, d->inf->ai_addr, &d->inf->ai_addrlen);
  if (new_sock == -1) {
    puts("Fucked up witn accepting");
    return -1;
  }
  puts("sock accepted");
  d->fd = fopen(d->fn, "w");
  puts("file opened");
  int a;
  while ( ( a = recv(new_sock, d->buf, d->packet_size, 0) ) > 0) {
    fwrite(d->buf, a, 1, d->fd);
  }
  goto _exit;
  } while (0);

_exit:
  return 0;
}
