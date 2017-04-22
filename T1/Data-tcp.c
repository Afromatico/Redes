#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include "jsocket6.h"
#include "bufbox.h"
#include "Data.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

int Data_debug = 0; /* para debugging */
double Ddelay, Dloss;

int Dconnect(char *server, char *port) {
    int s, cl, i;
    unsigned sz;
    struct sockaddr_storage from;
    struct sigaction new, old;
    unsigned char inbuf[DHDR], outbuf[DHDR];

    s = j_socket_tcp_connect(server, port);
    return s;
}

void Dbind(void* (*f)(void *), char *port) {
    int s, s2;
    int *p;

    s = j_socket_tcp_bind(port);
    if(s < 0) {
	fprintf(stderr, "bind failed\n");
	exit(1);
    }

    for(;;) {
	s2 = j_accept(s);
	p = (int *)malloc(sizeof(int));
	*p = s2;
	f((void *)p);
    }
}

int Dread(int cl, char *buf, int l) {
int cnt;
int sz;

    if(read(cl, &sz, 4) != 4) return -1;
    sz = ntohl(sz);
    if(sz > 0)
        return read(cl, buf, min(l,sz));
    else if(sz == 0)
	return 0;
    else return -1;
}

void Dwrite(int cl, char *buf, int l) {
int sz;

    sz = htonl(l);
    write(cl, &sz, 4);
    if(l > 0) write(cl, buf, l);
}

void Dclose(int cl) {
    close(cl);
}
