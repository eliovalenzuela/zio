
# build user-space tools for zio

HOST_EXTRACFLAGS += -I$(M)/include/

HOSTCC ?= gcc

hostprogs-y := zio-dump
hostprogs-y += zio-cat-file
hostprogs-y += test-dtc
hostprogs-y += zio-pipestamp

# The following is ugly, please forgive me by now
user: $(hostprogs-y)

clean:
	rm -f $(hostprogs-y) *~ *.o

%: %.c
	$(HOSTCC) $(HOST_EXTRACFLAGS) -Wall $^ -o $@

