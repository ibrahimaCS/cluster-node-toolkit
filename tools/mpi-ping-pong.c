/**
 * @file mpi-ping-pong.c
 * @brief MPI point-to-point latency and bandwidth benchmark.
 * @details Needs exactly 2 ranks. Rank 0 sends, rank 1 echoes back,
 *          rank 0 times the round trip. Sweeps message sizes from 1 B
 *          to 4 MiB in powers of two, under both blocking Send/Recv
 *          and non-blocking Isend/Irecv + Wait. Prints a latency and
 *          implied-bandwidth table.
 *          Usage:   mpirun -n 2 ./mpi-ping-pong
 *          Compile: mpicc -Wall -Wextra -Wpedantic -std=c11 -O2 mpi-ping-pong.c -o mpi-ping-pong
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_SIZE  1  // Smallest payload in bytes
#define MAX_SIZE  (4 * 1024 * 1024) // Largest payload: 4 MiB
#define WARMUP 32   // Iterations before timing
#define ITERS_SMALL 10000  // Iterations for latency-sized msgs
#define ITERS_LARGE 200  // Iterations for bandwidth-sized msgs


/**
 * @brief Blocking ping-pong under MPI_Send / MPI_Recv.
 * @return Average time per round trip in seconds.
 */
static double bench_blocking(int rank, char *buf, size_t sz, int iters) {
    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();

    for (int i = 0; i < iters; i++) {
        if (rank == 0) {
            MPI_Send(buf, (int)sz, MPI_BYTE, 1, 0, MPI_COMM_WORLD);
            MPI_Recv(buf, (int)sz, MPI_BYTE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            MPI_Recv(buf, (int)sz, MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(buf, (int)sz, MPI_BYTE, 0, 0, MPI_COMM_WORLD);
        }
    }

    double t1 = MPI_Wtime();
    return (t1 - t0) / (double)iters;
}

/**
 * @brief Non-blocking ping-pong under MPI_Isend / MPI_Irecv + MPI_Wait.
 * @return Average time per round trip in seconds.
 */
static double bench_nonblocking(int rank, char *buf, size_t sz, int iters) {
    MPI_Request req[2];
    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();

    for (int i = 0; i < iters; i++) {
        if (rank == 0) {
            MPI_Isend(buf, (int)sz, MPI_BYTE, 1, 0, MPI_COMM_WORLD, &req[0]);
            MPI_Irecv(buf, (int)sz, MPI_BYTE, 1, 0, MPI_COMM_WORLD, &req[1]);
            MPI_Waitall(2, req, MPI_STATUSES_IGNORE);
        } else {
            MPI_Irecv(buf, (int)sz, MPI_BYTE, 0, 0, MPI_COMM_WORLD, &req[0]);
            MPI_Wait(&req[0], MPI_STATUS_IGNORE);
            MPI_Isend(buf, (int)sz, MPI_BYTE, 0, 0, MPI_COMM_WORLD, &req[1]);
            MPI_Wait(&req[1], MPI_STATUS_IGNORE);
        }
    }

    double t1 = MPI_Wtime();
    return (t1 - t0) / (double)iters;
}


/**
 * @brief Program entry point.
 * @details Verifies two ranks, allocates the maximum buffer, sweeps
 *          message sizes and prints a table once per size.
 */
int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    // Enforce 2 ranks; a ping-pong with more or fewer makes no sense
    if (nranks != 2) {
        if (rank == 0)
            fprintf(stderr, "mpi-ping-pong: needs exactly 2 ranks, got %d\n", nranks);
        MPI_Finalize();
        return 1;
    }

    char *buf = malloc(MAX_SIZE);
    if (!buf) {
        perror("malloc");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    memset(buf, 'x', MAX_SIZE);

    if (rank == 0) {
        printf("mpi-ping-pong: 2 ranks, 1 B .. %d B\n", MAX_SIZE);
        printf("Size (B)   RTT_block (us)  BW_block (MB/s)  RTT_nb (us)  BW_nb (MB/s)\n");
    }

    // Sweep message sizes in powers of two
    for (size_t sz = MIN_SIZE; sz <= MAX_SIZE; sz *= 2) {
        // Smaller messages run more iterations for stable timing
        int iters = (sz < 4096) ? ITERS_SMALL : ITERS_LARGE;

        // Warm-up ignored; cache / interconnect pre-heated
        (void)bench_blocking(rank, buf, sz, WARMUP);
        double rtt_b = bench_blocking(rank, buf, sz, iters);

        (void)bench_nonblocking(rank, buf, sz, WARMUP);
        double rtt_n = bench_nonblocking(rank, buf, sz, iters);

        if (rank == 0) {
            // 2*sz because the round trip transfers the payload twice
            double bw_b = ((double)sz * 2.0) / rtt_b / (1024.0 * 1024.0);
            double bw_n = ((double)sz * 2.0) / rtt_n / (1024.0 * 1024.0);
            printf("%9zu  %14.3f  %14.2f  %11.3f  %11.2f\n",
                   sz, rtt_b * 1e6, bw_b, rtt_n * 1e6, bw_n);
        }
    }

    free(buf);
    MPI_Finalize();
    return 0;
}
