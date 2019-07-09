#include "listener.h"
void main (int argc, char** argv){
	Mandata* data  = parse_args(argc, argv);
	if (data == NULL) {
		puts("Initializing error");
		exit(0);
	}

	int res = exchange(data);
	if(res == -1 ) {
		puts("Smth went wrong with receiving data");
	}
	
	finish(data);

}

 int finish (Mandata* data) {
	puts("Finished receiving data");
	free(data->buf);
   free(data->inf);
   free(data);
   return 0;
 }

int exchange(Mandata* data){
	//нужно создать сокет
		  int sock = socket(data->inf->ai_family, data->inf->ai_socktype, data->inf->ai_protocol);
		  if (sock == -1){
			puts("failed to create socket");
			return -1;
		  }
puts("sock created");
	//теперь скажем на каком порте будем слушать
	
	int res = bind (sock ,data->inf->ai_addr, data->inf->ai_addrlen);
	if (res == -1) {
		puts("failed to bind socket");
		return -1;
	}
puts("sock binded");
	res = listen(sock, 1);
	if (res == -1){
		puts("Cannot hear you");
		return -1;
	}
puts("sock listened");
	int new_sock = accept(sock, data->inf->ai_addr, &data->inf->ai_addrlen);
	if(new_sock == -1){
		puts("Fucked up witn accepting");
		return -1;
	}
	puts("sock accepted");
	data->fd = fopen(data->fn, "w");
	puts("file opened");
	while(recv(new_sock, data->buf, data->packet_size, 0) > 0) {
			fwrite(data->buf, data->packet_size, 1, data->fd);
		}
	
	fclose(data->fd);
	return 0;
}




Mandata* parse_args(int argc, char** argv){
	if (argc < 3) {
		printf("Bad usage: \nTry %s port file_name\n", argv[0]);
		return NULL;
	}
	Mandata* data = malloc (sizeof(Mandata));
   data->packet_size = DEFAULT_PACKETSIZE;
   data->buf = malloc(data->packet_size);

	data->port = argv[1];
	data->fn = argv[2];
	data->fd = NULL;//Пока не буду создавать файл, мало ли никто ничего не пришлет

	data->inf = malloc(sizeof(struct addrinfo));
	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;//ftp
	hints.ai_flags = AI_PASSIVE;//Автоопределение IP слушателя

	int res = getaddrinfo(NULL, data->port, &hints, &data->inf); //В inf запишет всю нужную для сокета инфу

	if (res != 0) {
	free(data->inf);
	free(data->buf);
	free(data);
	perror("getaddrinfo");
	return NULL;
	}
	puts("parsed");
	return data;

}
