CPPFLAGS += -Ideps
CFLAGS   += -Wall -std=c11 -pthread
LDLIBS   += -pthread

C := spool.c spooldir.c \
	 $(wildcard deps/*/*.c)
O := $(patsubst %.c,%.o,$C)
E := spool

$E: $O
$O: deps

deps:
	clib install

all: $E

clean:
	$(RM) $O $E
