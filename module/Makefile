# $dellflash$

SRCS=biosmod.c
OBJS=$(SRCS:.c=.o)

KMOD=biosmod
CFLAGS+= -fPIC -fno-builtin -D_KERNEL -I/sys

all:	$(OBJS)

clean:
	rm -f $(OBJS) $(KMOD)

load:
	modload -o $(KMOD) -e$(KMOD) $(KMOD).o

unload:
	modunload -n $(KMOD)
