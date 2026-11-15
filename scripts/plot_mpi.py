#!/usr/bin/env python3
"""plot_mpi.py - plots for mpi-ping-pong and mpi-allreduce outputs."""
import sys, re
import matplotlib.pyplot as plt

def parse_pingpong(path):
    S, Rb, Bb, Rn, Bn = [], [], [], [], []
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', line)
            if m:
                S.append(int(m.group(1)))
                Rb.append(float(m.group(2))); Bb.append(float(m.group(3)))
                Rn.append(float(m.group(4))); Bn.append(float(m.group(5)))
    return S, Rb, Bb, Rn, Bn

def parse_allreduce(path):
    S, T, B = [], [], []
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*\d+\s+(\d+)\s+([\d.]+)\s+([\d.]+)', line)
            if m:
                S.append(int(m.group(1)))
                T.append(float(m.group(2))); B.append(float(m.group(3)))
    return S, T, B

def plot_pingpong(path):
    S, Rb, Bb, Rn, Bn = parse_pingpong(path)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.5))
    ax1.loglog(S, Rb, 'o-', label='Blocking (Send/Recv)')
    ax1.loglog(S, Rn, 's-', label='Non-blocking (Isend/Irecv)')
    ax1.set(xlabel='Message size (bytes)', ylabel='RTT (µs)',
            title='MPI point-to-point latency')
    ax1.grid(True, which='both', alpha=0.3); ax1.legend()
    ax2.semilogx(S, Bb, 'o-', label='Blocking')
    ax2.semilogx(S, Bn, 's-', label='Non-blocking')
    ax2.set(xlabel='Message size (bytes)', ylabel='Bandwidth (MB/s)',
            title='MPI point-to-point bandwidth')
    ax2.grid(True, which='both', alpha=0.3); ax2.legend()
    plt.tight_layout(); plt.savefig('mpi-ping-pong.png', dpi=120)
    print('Wrote mpi-ping-pong.png')

def plot_allreduce(paths):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.5))
    for path in sorted(paths):
        m = re.search(r'(\d+)', path)
        label = f'{m.group(1)} ranks' if m else path
        S, T, B = parse_allreduce(path)
        ax1.loglog(S, T, 'o-', label=label)
        ax2.semilogx(S, B, 'o-', label=label)
    ax1.set(xlabel='Elements (doubles)', ylabel='Time (µs)',
            title='MPI_Allreduce — time vs size')
    ax1.grid(True, which='both', alpha=0.3); ax1.legend()
    ax2.set(xlabel='Elements (doubles)', ylabel='Effective bandwidth (MB/s)',
            title='MPI_Allreduce — bandwidth vs size')
    ax2.grid(True, which='both', alpha=0.3); ax2.legend()
    plt.tight_layout(); plt.savefig('mpi-allreduce.png', dpi=120)
    print('Wrote mpi-allreduce.png')

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('Usage: plot_mpi.py pingpong <file>'); print('       plot_mpi.py allreduce <f1> <f2> ...')
        sys.exit(1)
    mode = sys.argv[1]
    if mode == 'pingpong':  plot_pingpong(sys.argv[2])
    elif mode == 'allreduce': plot_allreduce(sys.argv[2:])
    else: sys.exit(f'Unknown mode: {mode}')
    
