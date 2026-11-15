# =============================================================
#  Cluster Node Toolkit - top-level Makefile
# =============================================================

.PHONY: all tools mpi scripts clean check bench demo help logs preflight plots

all: tools mpi scripts logs plots

tools:
	$(MAKE) -C tools

mpi:
	$(MAKE) -C tools mpi

scripts:
	@chmod +x scripts/*.sh
	@echo "Scripts executable."

check: scripts
	@echo "=== ShellCheck ==="
	@for s in scripts/*.sh; do echo "--- $$s ---"; shellcheck $$s || true; done
	@echo
	@echo "=== Strict C recompile (-Werror + more) ==="
	@$(MAKE) -C tools clean >/dev/null
	@$(MAKE) -C tools CFLAGS="-Wall -Wextra -Wpedantic -Wshadow \
	    -Wconversion -std=c11 -O2 -Werror -D_GNU_SOURCE"
	@$(MAKE) -C tools mpi

bench: tools
	@echo "=== Timing observation tools ==="
	@for t in cpu-topology memory-inspector net-topology; do \
	    echo "--- ./tools/$$t ---"; \
	    { time ./tools/$$t >/dev/null; } 2>&1 | grep real; \
	done

demo: tools
	@echo "=== Echo server/client demo (3s) ==="
	@./tools/echo-server -p 19000 & \
	    sleep 0.2; \
	    ./tools/echo-client 127.0.0.1 19000 -n 1000 -s 64; \
	    kill %1 2>/dev/null || true

logs: tools mpi
	@mkdir -p logs
	@echo "=== Capturing every tool to logs/*.txt ==="
	@./tools/cpu-topology                        > logs/cpu-topology.txt
	@./tools/memory-inspector                    > logs/memory-inspector.txt
	@./tools/net-topology                        > logs/net-topology.txt
	@timeout 3 ./tools/bandwidth-monitor -i 1    > logs/bandwidth-monitor.txt 2>&1 || true
	@./tools/ring-latency -n 200000 -a 0 -b 2    > logs/ring-latency.txt
	@mpirun -n 2 ./tools/mpi-ping-pong           > logs/mpi-ping-pong.txt
	@for N in 2 4 8 16; do \
	    mpirun -n $$N ./tools/mpi-allreduce > logs/mpi-allreduce-$$N.txt; \
	done
	@./scripts/env-probe.sh                      > logs/env-probe.txt 2>&1 || true
	@echo "(packet-sniffer skipped: needs CAP_NET_RAW)"
	@echo "Done. $$(ls logs/*.txt 2>/dev/null | wc -l) files in logs/"

preflight: logs
	@echo "Preflight ready: see logs/ for the full node audit."

plots: logs
	@python3 scripts/plot_mpi.py pingpong  logs/mpi-ping-pong.txt
	@python3 scripts/plot_mpi.py allreduce logs/mpi-allreduce-*.txt
	@echo "Plots: mpi-ping-pong.png, mpi-allreduce.png"

clean:
	$(MAKE) -C tools clean
	rm -rf build/ logs/

help:
	@echo "Targets: all tools mpi scripts check bench demo logs preflight plots clean help"

