prefix=/usr/local
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Libs= -L${libdir} -lfuse -pthread
Cflags= -Wall -I${includedir}/fuse -D_FILE_OFFSET_BITS=64

CC=gcc

all:
	$(CC) $(Cflags) $(Libs) -lulockmgr -o chfs chfs.c
