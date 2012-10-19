
# build user-space tools for zio

HOST_EXTRACFLAGS += -I$(M)/include/

HOSTCC ?= gcc

hostprogs-y := zio-dump
hostprogs-y += zio-cat-file
hostprogs-y += pfzio-receive
hostprogs-y += pfzio-send

hostprogs-y += zio-perf-cdev
hostprogs-y += zio-perf-sock


# The following is ugly, please forgive me by now
user: $(hostprogs-y)

clean:
	rm -f $(hostprogs-y) *~ *.o

%: %.c
	$(HOSTCC) $(HOST_EXTRACFLAGS) -Wall $^ -o $@

