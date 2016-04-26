# If it exists includes Makefile.specific. In this Makefile, you should put
# specific Makefile code that you want to run before this. For example,
# build a particular environment.
-include Makefile.specific

ZIO ?= ../

CFLAGS = -Wall -Werror -ggdb -O2 -fPIC -I$(ZIO)/lib
CFLAGS += -I$(ZIO)/include $(EXTRACFLAGS)

LIBSHARE = libzio.so
LIBSTAT = libzio.a
LOBJ := libzio-print.o
LOBJ += libzio-sysfs.o
LOBJ += libzio-scan.o
LOBJ += libzio-utils.o
LOBJ += libzio-block.o
LOBJ += libzio-buffer.o
LOBJ += libzio-trigger.o

all: lib
lib: $(LIBSTAT) $(LIBSHARE)

%: %.c
	$(CC) $(CFLAGS) $*.c $(LDFLAGS) -o $@

$(LIBSTAT): $(LOBJ)
	ar r $@ $^
	ranlib $@

$(LIBSHARE): $(LIBSTAT)
		$(CC) -shared  -o $@ -Wl,--whole-archive,-soname,$@ $^ -Wl,--no-whole-archive
clean:
	rm -f *.a *.so .depend *.o *~
