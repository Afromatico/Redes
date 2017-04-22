# Para Linux agregar -lrt
CC=gcc
CFLAGS=-g # -m32

BIN=bwc-orig bwc-tcp bwc-new 
all: $(BIN)

bwc-orig: bwc.o jsocket6.4.o Dataclient-seqn.o jsocket6.4.h bufbox.o 
	$(CC) $(CFLAGS) bwc.o jsocket6.4.o Dataclient-seqn.o bufbox.o -o $@ -lpthread 

bwc-new: bwc.o jsocket6.4.o Dataclient-seqn-new.o jsocket6.4.h bufbox.o 
	$(CC) $(CFLAGS) bwc.o jsocket6.4.o Dataclient-seqn-new.o bufbox.o -o $@ -lpthread 

bwc-tcp: bwc.o jsocket6.4.o Data-tcp.o jsocket6.4.h
	$(CC) $(CFLAGS) bwc.o jsocket6.4.o Data-tcp.o -o $@ -lpthread 

clean: 
	rm -f $(BIN) *.o
	rm -f tmpbw*
