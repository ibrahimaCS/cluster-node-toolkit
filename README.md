# Cluster Node Toolkit

**Twelve small programs that look at a Linux server and report what it can
really do** — how many CPUs, how much memory, how fast the network, how
well the machine handles parallel computing. Run them before a big job,
before deploying a service, or when you need to understand why a server
is slow. In one minute, you get a complete picture of the machine, as
numbers rather than guesses.

## Why this exists

There are already great tools for each piece: `lscpu` for the CPU,
`iftop` for bandwidth, `tcpdump` for packets, `osu-micro-benchmarks` for
MPI. But they are scattered across many packages, each with its own
conventions and output formats. This project gathers the essentials into
one coherent set: a single `make`, one binary per question, no
dependencies beyond libc, pthread and MPI.

Companion project to **ARSE23 at ENSIIE**; portfolio piece for HPC,
systems, networking and telecom internships.

`[C11 + minimal Bash]` · `[Linux x86_64 / AArch64]` · `[Deps: libc, pthread, MPI]`


Two real situations where this toolkit helps:

1. **"I rented a server to run a big calculation."**
   Before spending the compute hours, you want to check: does the machine
   really have what was advertised? How fast is the memory, is the
   network delivering its promised bandwidth, are the huge memory pages
   enabled?

2. **"My server is slow and I don't know why."**
   Look at every network card, its real throughput, and the actual
   traffic — without touching production tools.


## The twelve tools, by family

- **A — System foundation.** `cpu-topology` (sockets, cores, SMT,
  caches, NUMA, cpufreq) · `memory-inspector` (RAM, hugepages, TLB
  reach, per-NUMA memory).
- **B — Network observability.** `net-topology` (interfaces, IPs,
  routes, ARP, IRQ affinity) · `bandwidth-monitor` (live RX/TX from
  `/proc/net/dev`) · `packet-sniffer` (`AF_PACKET` decoder for Ethernet
  / IPv4 / TCP / UDP / ICMP).
- **C — Concurrency.** `thread-pool.{h,c}` (POSIX pool with mutex and
  cond) · `ring-latency` (SPSC ring across pinned CPUs; demonstrates
  false sharing when `alignas(64)` is removed).
- **D — Sockets.** `echo-server` (`epoll` edge-triggered +
  `EPOLLONESHOT` + the thread pool from C) · `echo-client` (load
  generator reporting min/avg/p50/p99 RTT and aggregate bandwidth).
- **E — MPI.** `mpi-ping-pong` (point-to-point, blocking and
  non-blocking) · `mpi-allreduce` (collective benchmark against the
  ring-Allreduce theoretical bound).

## Prerequisites

```bash
# Debian / Ubuntu
sudo apt install build-essential libopenmpi-dev openmpi-bin shellcheck
# Fedora / RHEL
sudo dnf install gcc make openmpi-devel ShellCheck
```

**MPI is mandatory.** The build fails if `mpicc` is not on the path.

## Quickstart

```bash
git clone https://github.com/IbrahimaCS/cluster-node-toolkit.git
cd cluster-node-toolkit
./scripts/env-probe.sh    # exits non-zero if anything mandatory is missing
make                       # build everything + capture logs + render plots
make check                 # strict re-build + ShellCheck

./tools/cpu-topology
./tools/memory-inspector
./tools/net-topology
./tools/bandwidth-monitor eth0
sudo ./tools/packet-sniffer eth0
./tools/ring-latency -n 200000 -a 0 -b 2
./tools/echo-server -p 9000 -t 4 &
./tools/echo-client 127.0.0.1 9000 -n 10000 -s 1024
mpirun -n 2 ./tools/mpi-ping-pong
mpirun -n 4 ./tools/mpi-allreduce
```

## Safety

- Every tool except `packet-sniffer` is strictly read-only — nothing
  writes to `/proc` or `/sys`.
- `packet-sniffer` needs `CAP_NET_RAW`. Grant it without running the
  binary as root: `sudo setcap cap_net_raw+ep ./tools/packet-sniffer`.

## Repository layout

```
cluster-node-toolkit/
├─ Makefile              # top-level: all, mpi, check, bench, demo, logs, plots, clean
├─ scripts/
│   ├─ env-probe.sh      # single Bash script
│   └─ plot_mpi.py       # renders mpi-*.png from logs/
└─ tools/                # Makefile + 11 .c files + thread-pool.h
```

## Author

Built by Ibrahima, engineering student at ENSIIE.

