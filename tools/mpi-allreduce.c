/**
 * @file mpi-allreduce.c
 * @brief MPI_Allreduce benchmark (MPI_SUM on doubles).
 * @details Any communicator with 2 or more ranks. Sweeps message sizes
 *          from 1 to 1 M doubles (= 8 MiB), prints the average per-call
 *          time and the implied bandwidth. Compare the latter to the
 *          ring-Allreduce theoretical bound 2*(P-1)/P * message_size.
 *          Usage:   mpirun -n N ./mpi-allreduce
 *          Compile: mpicc -Wall -Wextra -Wpedantic -std=c11 -O2 mpi-allreduce.c -o mpi-allreduce
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_N   1
#define MAX_N   (1 << 20)   // 1 M doubles = 8 MiB
#define WARMUP  16
#define ITERS   200

/**
 * @brief Program entry point.
 */
int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    // Reduction is meaningless with a single rank
    if (nranks < 2) {
        if (rank == 0)
            fprintf(stderr, "mpi-allreduce: needs at least 2 ranks\n");
        MPI_Finalize();
        return 1;
    }

    double *sbuf = malloc(MAX_N * sizeof(double));
    double *rbuf = malloc(MAX_N * sizeof(double));
    if (!sbuf || !rbuf) {
        perror("malloc");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    // Fill the send buffer with a rank-specific value so the result is
    // easy to sanity-check (SUM over ranks of (rank+1))
    for (int i = 0; i < MAX_N; i++) sbuf[i] = (double)(rank + 1);

    if (rank == 0) {
        printf("mpi-allreduce: %d ranks, MPI_SUM on doubles\n", nranks);
        printf("Elements     Bytes         Time (us)       BW eff. (MB/s)\n");
    }

    // Sweep message sizes in powers of two
    for (size_t n = MIN_N; n <= MAX_N; n *= 2) {
        // Warm-up: fills caches, pre-connects every pair of ranks
        for (int w = 0; w < WARMUP; w++)
            MPI_Allreduce(sbuf, rbuf, (int)n, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

        MPI_Barrier(MPI_COMM_WORLD);
        double t0 = MPI_Wtime();
        for (int i = 0; i < ITERS; i++)
            MPI_Allreduce(sbuf, rbuf, (int)n, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        double t1 = MPI_Wtime();

        double per_op = (t1 - t0) / (double)ITERS;

        if (rank == 0) {
            size_t bytes = n * sizeof(double);
            // Ring-Allreduce lower bound: 2*(P-1)/P * payload per link
            double eff_bytes = 2.0 * (double)(nranks - 1) / (double)nranks * (double)bytes;
            double bw_MB    = eff_bytes / per_op / (1024.0 * 1024.0);
            printf("%10zu  %10zu    %13.3f    %14.2f\n",
                   n, bytes, per_op * 1e6, bw_MB);
        }
    }
    
    free(sbuf);
    free(rbuf);
    MPI_Finalize();
    return 0;
}

