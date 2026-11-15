/**
 * @file thread-pool.h
 * @brief Minimal POSIX thread pool interface.
 * @details Fixed number of workers created at tpool_create. A single
 *          FIFO queue protected by a mutex; workers block on a
 *          condition variable while the queue is empty. tpool_destroy
 *          waits for every queued task to be consumed, signals stop
 *          and joins every worker. All functions are thread-safe.
 */

#ifndef TPOOL_H
#define TPOOL_H

#include <stddef.h>

/**
 * @brief Opaque handle on a thread pool instance.
 */
typedef struct tpool tpool_t;

/**
 * @brief Function signature of a submitted task.
 * @param arg Opaque pointer passed by the submitter.
 */
typedef void (*tpool_task_fn)(void *arg);

/**
 * @brief Creates a pool with @p nthreads worker threads.
 * @param nthreads Number of workers (must be at least 1).
 * @return Pool handle, or NULL on allocation / pthread_create failure.
 */
tpool_t *tpool_create(size_t nthreads);

/**
 * @brief Submits a task to the pool.
 * @param p   Pool handle.
 * @param fn  Task function.
 * @param arg Argument forwarded to @p fn.
 * @return 0 on success, -1 if the pool is stopped or allocation fails.
 */
int tpool_submit(tpool_t *p, tpool_task_fn fn, void *arg);

/**
 * @brief Drains the queue, stops the pool, joins workers, frees memory.
 * @param p Pool handle. Safe to call with NULL.
 */
void tpool_destroy(tpool_t *p);

#endif

