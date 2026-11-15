/**
 * @file ring-latency.c
 * @brief Inter-thread latency benchmark on a SPSC ring buffer.
 * @details A producer on CPU A writes a monotonic timestamp into a
 *          ring slot; a consumer on CPU B reads it and records
 *          (now - sent) as the one-way latency. The ring's head and
 *          tail indices sit on separate cache lines (alignas(64)) to
 *          avoid false sharing - remove those keywords, recompile,
 *          and watch p50 jump by a factor of 5 to 20.
 *          Usage:   ./ring-latency [-n samples] [-a cpuA] [-b cpuB]
 *          Compile: gcc -Wall -Wextra -Wpedantic -std=c11 -O2 -D_GNU_SOURCE \
 *                       ring-latency.c -o ring-latency -lpthread
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>

// Common x86_64 cache line size; the right default on most Intel/AMD CPUs
#define CACHELINE 64

// Ring must be a power of two to enable the mask trick for wrap-around
#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)

/**
 * @brief Single-producer / single-consumer lock-free ring buffer.
 * @details Head is written by the producer, tail by the consumer.
 *          Pads ensure each index owns an exclusive cache line.
 */
typedef struct {
    alignas(CACHELINE) _Atomic uint64_t head;
    char _pad1[CACHELINE - sizeof(uint64_t)];
    alignas(CACHELINE) _Atomic uint64_t tail;
    char _pad2[CACHELINE - sizeof(uint64_t)];
    alignas(CACHELINE) uint64_t buf[RING_SIZE];
} ring_t;

// Shared state kept in file scope to avoid cluttering thread args
static ring_t g_ring;
static long  g_samples = 200000;
static int  g_cpu_a  = 0;
static int g_cpu_b  = 1;
static uint64_t *g_lat;

/**
 * @brief Returns the current monotonic time in nanoseconds.
 */
static inline uint64_t now_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Pins the current thread to CPU @p cpu.
 * @return 0 on success, non-zero on failure.
 */
static int pin_self(int cpu){
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((size_t)cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/**
 * @brief Producer thread: writes timestamps into successive ring slots.
 */
static void *producer(void *arg){
    (void)arg;
    if (pin_self(g_cpu_a) != 0)
        fprintf(stderr, "warn: cannot pin producer to CPU %d\n", g_cpu_a);

    for (long i = 0; i < g_samples; i++) {
        uint64_t h = atomic_load_explicit(&g_ring.head, memory_order_relaxed);

        // Spin-wait while the ring is full
        while (h - atomic_load_explicit(&g_ring.tail, memory_order_acquire) >= RING_SIZE) {
            /* spin */
        }

        g_ring.buf[h & RING_MASK] = now_ns();
        atomic_store_explicit(&g_ring.head, h + 1, memory_order_release);
    }
    return NULL;
}


/**
 * @brief Consumer thread: reads each timestamp, records the elapsed ns.
 */
static void *consumer(void *arg){
    (void)arg;
    if (pin_self(g_cpu_b) != 0)
        fprintf(stderr, "warn: cannot pin consumer to CPU %d\n", g_cpu_b);

    for (long i = 0; i < g_samples; i++) {
        uint64_t t = atomic_load_explicit(&g_ring.tail, memory_order_relaxed);

        // Spin-wait while the ring is empty
        while (t == atomic_load_explicit(&g_ring.head, memory_order_acquire)) {
            /* spin */
        }

        uint64_t sent = g_ring.buf[t & RING_MASK];
        uint64_t recv = now_ns();
        g_lat[i] = recv - sent;
        atomic_store_explicit(&g_ring.tail, t + 1, memory_order_release);
    }
    return NULL;
}


/**
 * @brief qsort comparator for uint64_t values.
 */
static int cmp_u64(const void *a, const void *b){
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}


/**
 * @brief Program entry point.
 * @details Parses arguments, allocates the latency array, spawns both
 *          threads, sorts samples, and prints min/avg/p50/p99/max.
 */
int main(int argc, char *argv[]){
    // --- Parse command-line arguments ---
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i+1 < argc) g_samples = atol(argv[++i]);
        else if (strcmp(argv[i], "-a") == 0 && i+1 < argc) g_cpu_a = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) g_cpu_b = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-n samples] [-a cpuA] [-b cpuB]\n", argv[0]);
            return 0;
        }
    }
    printf("ring-latency: %ld samples, producer CPU %d, consumer CPU %d\n",
            g_samples, g_cpu_a, g_cpu_b);

    g_lat = malloc((size_t)g_samples * sizeof(uint64_t));
    if (!g_lat) { perror("malloc"); return 1; }

    pthread_t tp, tc;
    // Start consumer first so no sample is lost if producer is fast
    if (pthread_create(&tc, NULL, consumer, NULL) != 0) { perror("consumer"); return 1; }
    if (pthread_create(&tp, NULL, producer, NULL) != 0) { perror("producer"); return 1; }
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);

    // Sort to compute percentiles
    qsort(g_lat, (size_t)g_samples, sizeof(uint64_t), cmp_u64);
    uint64_t min = g_lat[0];
    uint64_t med = g_lat[g_samples / 2];
    uint64_t p99 = g_lat[(long)(0.99 * (double)g_samples)];
    uint64_t max = g_lat[g_samples - 1];

    double sum = 0.0;
    for (long i = 0; i < g_samples; i++) sum += (double)g_lat[i];
    double avg = sum / (double)g_samples;

    printf("\nLatency (nanoseconds):\n");
    printf("  min : %lu\n", (unsigned long)min);
    printf("  avg : %.1f\n", avg);
    printf("  p50 : %lu\n", (unsigned long)med);
    printf("  p99 : %lu\n", (unsigned long)p99);
    printf("  max : %lu\n", (unsigned long)max);
    printf("\nNote: head and tail are cache-line-aligned to avoid false sharing.\n");

    free(g_lat);
    return 0;
}

