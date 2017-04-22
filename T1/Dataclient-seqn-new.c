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
#include <signal.h>
#include <string.h>
#include "jsocket6.h"
#include "bufbox.h"
#include "Data-seqn.h"

#define n -4
#define MAX_QUEUE 100 /* buffers en boxes */

/* Version con threads rdr y sender
 * Implementa Stop and Wait sin números de secuencia
 * Timeouts fijos
 */


int Data_debug = 0; /* para debugging */

/* Variables globales del cliente */
static int Dsock = -1;
static pthread_mutex_t Dlock;
static pthread_cond_t  Dcond;
static pthread_t Drcvr_pid, Dsender_pid;
static unsigned char ack[DHDR] = {0, ACK,'0','0','0','0'};

static void *Dsender(void *ppp);
static void *Drcvr(void *ppp);


#define max(a, b) (((a) > (b))?(a):(b))

/* Variables conexion */

    BUFBOX *rbox, *wbox; /* Cajas de comunicación con el thread "usuario" */
    unsigned char pending_buf[BUF_SIZE]; /* buffer esperando ack */
    int pending_sz;  			 /* tamaño buffer esperando */
    int expecting_ack;			 /* 1: esperando ack */
    int retries;                         /* cuantas veces he retransmitido */
    double timeout;                      /* tiempo restante antes de retransmision */
    int state;                           /* FREE, CONNECTED, CLOSED */
    int id;                              /* id conexión, la asigna el servidor */

/* Funciones utilitarias */

/* retorna hora actual */
double Now() {
    struct timespec tt;
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    tt.tv_sec = mts.tv_sec;
    tt.tv_nsec = mts.tv_nsec;
#else
    clock_gettime(CLOCK_REALTIME, &tt);
#endif

    return(tt.tv_sec+1e-9*tt.tv_nsec);
}

/* Inicializa conexión */
void init_connection(int c_id) {

    state = CONNECTED;
    wbox = create_bufbox(MAX_QUEUE); 
    rbox = create_bufbox(MAX_QUEUE);
    pending_sz = -1;
    expecting_ack = 0;
    id = c_id;
    return;
}

/* borra conexión */
void del_connection() {
    delete_bufbox(wbox);
    delete_bufbox(rbox);
    state = FREE;
}

/* Función que inicializa los threads necesarios: sender y rcvr */
static void Init_Dlayer(int s) {

    Dsock = s;
    if(pthread_mutex_init(&Dlock, NULL) != 0) fprintf(stderr, "mutex NO\n");
    if(pthread_cond_init(&Dcond, NULL) != 0)  fprintf(stderr, "cond NO\n");
    pthread_create(&Dsender_pid, NULL, Dsender, NULL);
    pthread_create(&Drcvr_pid, NULL, Drcvr, NULL);
}

/* timer para el timeout */
void tick() {
    return;
}

/* Función que me conecta al servidor e inicializa el mundo */
int Dconnect(char *server, char *port) {
	if(Data_debug){fprintf(stderr,"Estoy en Dconnect\n");}
    int s, cl, i;
    struct sigaction new, old;
    unsigned char inbuf[DHDR], outbuf[DHDR];

    if(Dsock != -1) return -1;

    s = j_socket_udp_connect(server, port); /* deja "conectado" el socket UDP, puedo usar recv y send */
    if(s < 0) return s;

/* inicializar conexion */
    bzero(&new, sizeof new);
    new.sa_flags = 0;
    new.sa_handler = tick;
    sigaction(SIGALRM, &new, &old);

    outbuf[DTYPE] = CONNECT;
    outbuf[DID] = 0;
	outbuf[ACK1] = 48;
	outbuf[ACK2] = 48;
	outbuf[ACK3] = 48;
	outbuf[ACK4] = 48;

    for(i=0; i < RETRIES; i++) {
        send(s, outbuf, DHDR, 0);
	alarm(INTTIMEOUT);
	if(recv(s, inbuf, DHDR, 0) != DHDR) continue;
	if(Data_debug) fprintf(stderr, "recibo: %c, %d, \n", inbuf[DTYPE], inbuf[DID]);
	alarm(0);
	if(inbuf[DTYPE] != ACK) continue;
	cl = inbuf[DID];
	break;
    }
    sigaction(SIGALRM, &old, NULL);
    if(i == RETRIES) {
	fprintf(stderr, "no pude conectarme\n");
	return -1;
    }
fprintf(stderr, "conectado con id=%d\n", cl);
    init_connection(cl);
    Init_Dlayer(s); /* Inicializa y crea threads */
    return cl;
}

/* Lectura */
int Dread(int cl, char *buf, int l) {

	if(Data_debug){fprintf(stderr,"Estoy en Dread\n");}
	int cnt;

    if(id != cl) return -1;

    cnt = getbox(rbox, buf, l);
    return cnt;
}

/* escritura */
void Dwrite(int cl, char *buf, int l) {
	
	if(Data_debug){fprintf(stderr,"Estoy en Dwrite\n");}
    if(id != cl || state != CONNECTED) return;

    putbox(wbox, buf, l);
/* el lock parece innecesario, pero se necesita:
 * nos asegura que Dsender está esperando el lock o el wait
 * y no va a perder el signal! 
 */
    pthread_mutex_lock(&Dlock);
    pthread_cond_signal(&Dcond); 	/* Le aviso a sender que puse datos para él en wbox */
    pthread_mutex_unlock(&Dlock);
}

/* cierra conexión */
void Dclose(int cl) {
	
	if(Data_debug){fprintf(stderr,"Estoy en Dclose\n");}
    if(id != cl) return;

    close_bufbox(wbox);
    close_bufbox(rbox);
}

/*
 * Aquí está toda la inteligencia del sistema: 
 * 2 threads: receptor desde el socket y enviador al socket 
 */

/* lector del socket: todos los paquetes entrantes */
static void *Drcvr(void *ppp) { 
	
	if(Data_debug){fprintf(stderr,"Estoy en Drcvr\n");}
    int cnt;
    int cl, p;
    unsigned char inbuf[BUF_SIZE];
    int found;
	int j = 0;
	int k = 1;
	int Num_seq[9999];
	char str[2];
	str[1] = 0; 
/* Recibo paquete desde el socket */
    while((cnt=recv(Dsock, inbuf, BUF_SIZE, 0)) > 0) {
		str[0] = inbuf[ACK1];
		Num_seq[j] = atoi(str)*1000;
		str[0] = inbuf[ACK2];
		Num_seq[j] += atoi(str)*100;
		str[0] = inbuf[ACK3];
		Num_seq[j] += atoi(str)*10;
		str[0] = inbuf[ACK4];
		Num_seq[j] += atoi(str);
	/*	if(Data_debug){
			int m;
			for(m = 0; m < BUF_SIZE;m++){fprintf(stderr,"%c",inbuf[m]);}
		 }*/
  
   	if(Data_debug)
	    fprintf(stderr, "Drecv: id=%d, type=%c, Num_seq: %d\n", inbuf[DID], inbuf[DTYPE],Num_seq[j]);
	if(cnt < DHDR) continue; 

	cl = inbuf[DID];
   	if(cl != id) continue;
	
	pthread_mutex_lock(&Dlock);
	++j;
	if(inbuf[DTYPE] == CLOSE){ 
		if(Data_debug){
			 fprintf(stderr, "recibo cierre conexión %d, envío ACK\n", cl);
			 fprintf(stderr,"if 1\n");
		}
			ack[ACK1] = 48 + k/1000;
			ack[ACK2] = 48 + (k -(k/1000)*1000)/100;
			ack[ACK3] = 48 + (k -(k/100)*100)/10;
			ack[ACK4] = 48 + (k -(k/10)*10);
			k = (k+1)%9999;
			ack[DID] = cl;
			ack[DTYPE] = ACK;

			/*ack[ACK1] = inbuf[ACK1];
			ack[ACK2] = inbuf[ACK2];
			ack[ACK3] = inbuf[ACK3];
			ack[ACK4] = inbuf[ACK4];*/
			if(send(Dsock, ack, DHDR, 0) < 0) {
				perror("send"); exit(1);
			}
		

			state = CLOSED;
			Dclose(cl);
	}
	else if(inbuf[DTYPE] == ACK && state != FREE
		&& expecting_ack) {
		fprintf(stderr,"if 2\n");
	    if(Data_debug)
		fprintf(stderr, "Drecv ACK id=%d\n", cl);

	    expecting_ack = 0;
	    if(state == CLOSED) {
		/* conexion cerrada y sin buffers pendientes */
		del_connection();
	    }
	    pthread_cond_signal(&Dcond);
	}
	else if(inbuf[DTYPE] == DATA && state == CONNECTED) {
	    if(Data_debug){ 
			fprintf(stderr, "Drcv: DATA: %d, length=%d\n", inbuf[DID],cnt-DHDR);
			fprintf(stderr,"if 3\n");
	    }
		if(boxsz(rbox) >= MAX_QUEUE) { /* No tengo espacio */
			pthread_mutex_unlock(&Dlock);
			fprintf(stderr,"if 4\n");
			continue;
	    }
		ack[ACK1] = 48 + k/1000;
		ack[ACK2] = 48 + (k -(k/1000)*1000)/100;
		ack[ACK3] = 48 + (k -(k/100)*100)/10;
		ack[ACK4] = 48 + (k -(k/10)*10);
		k = (k+1)%9999;
	    ack[DID] = cl;
	    ack[DTYPE] = ACK;
	/*	ack[ACK1] = inbuf[ACK1];
		ack[ACK2] = inbuf[ACK2];
		ack[ACK3] = inbuf[ACK3];
		ack[ACK4] = inbuf[ACK4];*/
	    if(Data_debug) fprintf(stderr, "Drcvr: Enviando ACK %d\n", ack[DID]);

	    if(send(Dsock, ack, DHDR, 0) <0)
		perror("sendack");

	/* enviar a la cola */
		
		if(Data_debug){
			int m;
			for(m = 0; m < BUF_SIZE;m++){fprintf(stderr,"%c",inbuf[m]);}
		 }
	    putbox(rbox, (char *)inbuf + DHDR, cnt - DHDR);
		fprintf(stderr,"if 5\n");
        }
	else if(Data_debug) {
	    fprintf(stderr, "Drcvr: descarto paquete entrante: t=%c, id=%d\n", inbuf[DTYPE], inbuf[DID]);
	}
	
	pthread_mutex_unlock(&Dlock);
    }
    fprintf(stderr, "fallo read en Drcvr()\n");
    return NULL;
}

double Dclient_timeout_or_pending_data() {
	
	if(Data_debug){fprintf(stderr,"Estoy en Dclient_timeout_or_pending_data\n");}
    int cl, p;
    double ntimeout;
/* Suponemos lock ya tomado! */

    ntimeout = Now()+20.0;
	if(state == FREE) return ntimeout;

	if(boxsz(wbox) != 0 && !expecting_ack)
	/* data from client */
	    return Now();

    if(!expecting_ack)
	    return ntimeout;

	if(timeout <= Now()) return Now();
	if(timeout < ntimeout) ntimeout = timeout;
    return ntimeout;
}

/* Thread enviador y retransmisor */
static void *Dsender(void *ppp) { 
	if(Data_debug){fprintf(stderr,"Estoy en Dsender\n");}
	
    double ntimeout;
    struct timespec tt;
    int p;
    int ret;
	int j = 1;
  	
    for(;;) {
		pthread_mutex_lock(&Dlock);
        /* Esperar que pase algo */
	while((ntimeout=Dclient_timeout_or_pending_data()) > Now()) {
 // fprintf(stderr, "timeout=%f, now=%f\n", ntimeout, Now());
// fprintf(stderr, "Al tuto %f segundos\n", ntimeout-Now());
	    tt.tv_sec = ntimeout;
// fprintf(stderr, "Al tuto %f nanos\n", (ntimeout-tt.tv_sec*1.0));
	    tt.tv_nsec = (ntimeout-tt.tv_sec*1.0)*1000000000;
// fprintf(stderr, "Al tuto %f segundos, %d secs, %d nanos\n", ntimeout-Now(), tt.tv_sec, tt.tv_nsec);
	    ret=pthread_cond_timedwait(&Dcond, &Dlock, &tt);
// fprintf(stderr, "volvi del tuto con %d\n", ret);
	}
	if(Data_debug){fprintf(stderr," Dsender: expecting ack: %d, timeout: %f, Now: %f\n",expecting_ack,timeout,Now());}

	/* Revisar clientes: timeouts y datos entrantes */

	    if(state == FREE) continue;
            if(expecting_ack && timeout < Now()) { /* retransmitir */

                if(Data_debug) fprintf(stderr, " Dsender: TIMEOUT\n");

                if(++retries > RETRIES) {
                    fprintf(stderr, "Dsender: too many retries: %d\n", retries);
                    del_connection();
                    exit(1);
                }
                if(Data_debug) fprintf(stderr, " Dsender: Re-send DATA %d, length=%d\n", pending_buf[DID], pending_sz);
                if(send(Dsock, (char*)pending_buf, DHDR+pending_sz, 0) < 0) {
                    fprintf(stderr, "failed send length=%d\n", DHDR+pending_sz);
                    perror("send2"); exit(1);
                }
                timeout = Now() + TIMEOUT;

            }
	    if(boxsz(wbox) != 0 && !expecting_ack) {
/*
		Hay un buffer para mi para enviar y ya recibí el ACK para el último transmitido:
		leerlo, enviarlo, y marcar esperando ACK
*/
		pending_sz = getbox(wbox, (char *)pending_buf+DHDR+0, BUF_SIZE); 
		pending_buf[DID]=id;
		if(pending_sz == -1) { /* EOF */ 
		if(Data_debug) fprintf(stderr, " Dsender: sending EOF\n");
		   state = CLOSED;
		   pending_buf[DTYPE]=CLOSE;
		   pending_sz = 0;
		}
		else {
		   if(Data_debug) 
			fprintf(stderr, "sending DATA id=%d, length=%d\n", id, pending_sz);
		   pending_buf[DTYPE] = DATA;
			/* codigo nuevo: agrego los 4 bytes*/
			pending_buf[ACK1] = 48 + j/1000;
			pending_buf[ACK2] = 48 + (j -(j/1000)*1000)/100;
			pending_buf[ACK3] = 48 + (j -(j/100)*100)/10;
			pending_buf[ACK4] = 48 + (j -(j/10)*10);
			if(!Data_debug){
				int m;
				for(m = 0; m < BUF_SIZE;m++){fprintf(stderr,"%c",pending_buf[m]);}
				fprintf(stderr," Dsender: j = %d, ACK: (%c,%c,%c,%c)\n",j,pending_buf[ACK1],pending_buf[ACK2],pending_buf[ACK3],pending_buf[ACK4]);
		 }
			j = (j+1)%9999;
			if(pending_sz == 0){
				j = 0;
			//	pending_sz = 0;
			}
		
			
		}
		fprintf(stderr,"pending_sz: %d\n",pending_sz);
		send(Dsock, pending_buf, DHDR+pending_sz, 0);
		expecting_ack = 1;
		timeout = Now() + TIMEOUT;
		retries = 0;
	    }
	
	pthread_mutex_unlock(&Dlock);
    }
    return NULL;
}
