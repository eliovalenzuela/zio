
# build user-space tools for zio

CFLAGS = -I$(M)/include/ -I$(M)/lib -Wall $(ZIO_VERSION) $(EXTRACFLAGS)
CFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
LDFLAGS := -L$(M)/lib
LDLIBS += -Wl,-Bstatic -lzio
LDLIBS += -Wl,-Bdynamic

CC ?= $(CROSS_COMPILE)gcc

progs := zio-dump
progs += zio-cat-file
progs += test-dtc
progs += lszio

all : user
# The following is ugly, please forgive me by now
user: $(progs)

clean:
	rm -f $(progs) *~ *.o

%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
