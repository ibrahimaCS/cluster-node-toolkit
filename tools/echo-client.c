/**
 * @file echo-client.c
 * @brief TCP client: sends N messages of S bytes and measures RTT.
 * @details Opens a TCP connection with TCP_NODELAY, sends a payload of
 *          configurable size, waits for the echo, times the round trip
 *          with CLOCK_MONOTONIC and prints min / avg / p50 / p99 / max
 *          plus aggregate (RX+TX) bandwidth. Companion to echo-server.
 *          Usage:   ./echo-client HOST PORT [-n count] [-s size]
 *          Compile: gcc -Wall -Wextra -Wpedantic -std=c11 -O2 echo-client.c -o echo-client
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

/**
 * @brief Returns the current monotonic time in nanoseconds.
 */
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief qsort comparator for uint64_t values.
 */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}

/**
 * @brief Writes exactly @p n bytes, retrying on partial writes and EINTR.
 * @return @p n on success, -1 on write error.
 */
static ssize_t write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)n;
}


/**
 * @brief Reads exactly @p n bytes or returns short on EOF.
 * @return @p n on success, bytes read on peer close, -1 on error.
 */
static ssize_t read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return (ssize_t)(n - left);
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)n;
}


/**
 * @brief Program entry point.
 */
int main(int argc, char *argv[]){
    if (argc < 3) {
        fprintf(stderr, "Usage: %s HOST PORT [-n count] [-s size]\n", argv[0]);
        return 1;
    }

    const char *host  = argv[1];
    int port  = atoi(argv[2]);
    long count = 1000;
    size_t size  = 64;

    // --- Parse optional arguments ---
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i+1 < argc) count = atol(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) size = (size_t)atol(argv[++i]);
    }

    // --- Connect ---
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    // Disable Nagle to see the raw latency path
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port)
    };
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Bad IPv4: %s\n", host);
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }

    printf("echo-client: %s:%d, %ld messages of %zu bytes\n", host, port, count, size);

    // --- Allocate buffers ---
    char *sbuf = malloc(size);
    char *rbuf = malloc(size);
    if (!sbuf || !rbuf) { perror("malloc"); return 1; }
    memset(sbuf, 'x', size);

    uint64_t *lat = calloc((size_t)count, sizeof(uint64_t));
    if (!lat) {
        perror("malloc");
        return 1;
    }

        // --- Measurement loop ---
    long done = 0;
    uint64_t t_start = now_ns();
    for (long i = 0; i < count; i++) {
        uint64_t t0 = now_ns();
        if (write_all(fd, sbuf, size) < 0) {
            perror("write");
            break;
        }
        if (read_all(fd, rbuf, size) != (ssize_t)size) {
            fprintf(stderr, "server closed at message %ld (short read)\n", i);
            break;
        }
        lat[i] = now_ns() - t0;
        done++;
    }
    uint64_t t_end = now_ns();
    close(fd);

    if (done == 0) {
        fprintf(stderr, "No samples collected.\n");
        free(sbuf); free(rbuf); free(lat);
        return 1;
    }

    // --- Compute percentiles ---
        qsort(lat, (size_t)done, sizeof(uint64_t), cmp_u64);
    double sum = 0.0;
    for (long i = 0; i < done; i++) sum += (double)lat[i];

    double total_s = (double)(t_end - t_start) / 1e9;
    // Each round trip transfers 2*size bytes on the wire
    double bw_MB = ((double)done * (double)size * 2.0) / total_s / (1024.0 * 1024.0);

    printf("\nRTT over %ld successful round trips (nanoseconds):\n", done);
    printf("  min : %lu\n", (unsigned long)lat[0]);
    printf("  avg : %.1f\n", sum / (double)done);
    printf("  p50 : %lu\n", (unsigned long)lat[done/2]);
    printf("  p99 : %lu\n", (unsigned long)lat[(long)(0.99 * (double)done)]);
    printf("  max : %lu\n", (unsigned long)lat[done-1]);
    printf("\nAggregate (RX+TX) bandwidth: %.2f MB/s\n", bw_MB);

    free(sbuf);
    free(rbuf);
    free(lat);
    return 0;
}

