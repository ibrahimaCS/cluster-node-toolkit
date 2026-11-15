# =============================================================
#  tools/Makefile - build all C tools
#  MPI is mandatory: `make mpi` fails hard if mpicc is missing.
# =============================================================

CC      ?= gcc
MPICC   ?= mpicc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -D_GNU_SOURCE
LDLIBS_PTHREAD = -lpthread

TOOLS = cpu-topology \
        memory-inspector \
        net-topology \
        bandwidth-monitor \
        packet-sniffer \
        ring-latency \
        echo-server \
        echo-client

MPI_TOOLS = mpi-ping-pong mpi-allreduce

.PHONY: all mpi clean

all: $(TOOLS)

# Default rule for simple single-file tools.
%: %.c
	$(CC) $(CFLAGS) $< -o $@

# Tools that need pthread.
ring-latency: ring-latency.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS_PTHREAD)

# echo-server depends on the thread-pool object.
thread-pool.o: thread-pool.c thread-pool.h
	$(CC) $(CFLAGS) -c thread-pool.c -o thread-pool.o

echo-server: echo-server.c thread-pool.o thread-pool.h
	$(CC) $(CFLAGS) echo-server.c thread-pool.o -o echo-server $(LDLIBS_PTHREAD)

# MPI targets (mandatory).
mpi: $(MPI_TOOLS)

mpi-ping-pong: mpi-ping-pong.c
	@command -v $(MPICC) >/dev/null 2>&1 || \
	  { echo "ERROR: mpicc not found (mandatory)"; exit 1; }
	$(MPICC) $(CFLAGS) $< -o $@

mpi-allreduce: mpi-allreduce.c
	@command -v $(MPICC) >/dev/null 2>&1 || \
	  { echo "ERROR: mpicc not found (mandatory)"; exit 1; }
	$(MPICC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TOOLS) $(MPI_TOOLS) *.o


