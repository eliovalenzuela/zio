# If it exists includes Makefile.specific. In this Makefile, you should put
# specific Makefile code that you want to run before this. For example,
# build a particular environment.
-include Makefile.specific

ZIO ?= ../

CFLAGS = -Wall -ggdb -O2 -fPIC -I$(ZIO)/lib -I$(ZIO)/include $(EXTRACFLAGS)

LIBSHARE = libzio.so
LIBSTAT = libzio.a
LOBJ := libzio-print.o

lib: $(LIBSTAT) $(LIBSHARE)

%: %.c $(LIBSTAT)
	$(CC) $(CFLAGS) $*.c $(LDFLAGS) -o $@

$(LIBSTAT): $(LOBJ)
	ar r $@ $^

$(LIBSHARE): $(LIBSTAT)
		$(CC) -shared  -o $@ -Wl,--whole-archive,-soname,$@ $^ -Wl,--no-whole-archive
clean:
	rm -f *.a *.so .depend *.o *~
