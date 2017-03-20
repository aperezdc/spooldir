CPPFLAGS += -Ideps
CFLAGS   += -Wall -std=c11 -pthread $(OPT_CFLAGS)
LDLIBS   += -pthread

H := spooldir.h dbg.h
C := spool.c spooldir.c \
	 $(wildcard deps/*/*.c)
O := $(patsubst %.c,%.o,$C)
E := spool

$E: $O
$O: deps $H

deps:
	clib install

all: $E

clean:
	$(RM) $O $E
