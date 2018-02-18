#
# Makefile 
#
CFLAGS=-Wall -Wstrict-prototypes -g

SOURCE= \
		rtl2udp.c \
		cJSON.c \
		cJSON.h \

OBJECT= \
		rtl2udp.o \
		cJSON.o

all: rtl2udp

rtl2udp: $(OBJECT)
	$(CC) -o rtl2udp $(OBJECT) -lm

install: rtl2udp
	cp rtl2udp /usr/local/bin

.c.o:
	$(CC) $(CFLAGS) -c $<
		

