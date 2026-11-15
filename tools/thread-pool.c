/**
 * @file thread-pool.c
 * @brief Implementation of the POSIX thread pool declared in thread-pool.h.
 * @details Compile: gcc -Wall -Wextra -Wpedantic -std=c11 -O2 -c thread-pool.c
 *          Link    : -lpthread
 */

#define _POSIX_C_SOURCE 200809L
#include "thread-pool.h"

#include <pthread.h>
#include <stdlib.h>

/**
 * @brief Linked-list node for the FIFO queue of pending tasks.
 */
typedef struct task {
    tpool_task_fn fn;
    void *arg;
    struct task *next;
} task_t;


/**
 * @brief Internal pool state.
 */
struct tpool{
    pthread_t *workers;  /**< Array of worker thread handles. */
    size_t nworkers;   /**< Number of entries in workers.*/

    pthread_mutex_t  mtx; /**< Guards head/tail/qlen/active.*/
    pthread_cond_t   not_empty;  /**< Workers block here when queue is empty.*/
    pthread_cond_t   drained;  /**< Main waits here on destroy.*/

    task_t *head, *tail;/**< FIFO head and tail.*/
    size_t qlen;  /**< Number of queued tasks.*/
    size_t active; /**< Workers currently running a task.*/
    int  stop; /**< Flag: stop as soon as queue drains. */
};


/**
 * @brief Main loop of every worker thread.
 * @details Blocks on the condition variable while the queue is empty;
 *          dequeues one task at a time; runs it outside the lock to
 *          avoid holding the mutex across user code; signals the main
 *          thread when the pool is fully drained.
 */
static void *worker_loop(void *arg){
    struct tpool *p = (struct tpool *)arg;

    for(;;){
        pthread_mutex_lock(&p->mtx);

        // Wait while the pool is running but has nothing to do.
        // The 'while' (not 'if') protects against spurious wakeups.
        while(!p->stop && !p->head){
            pthread_cond_wait(&p->not_empty, &p->mtx);
        }

        // Terminate when told to stop AND the queue is empty
        if (p->stop && !p->head) {
            pthread_mutex_unlock(&p->mtx);
            break;
        }

        // Dequeue one task
        task_t *t = p->head;
        p->head = t->next;
        if (!p->head) p->tail = NULL;
        p->qlen--;
        p->active++;

        pthread_mutex_unlock(&p->mtx);

        // Run the task without holding the lock
        t->fn(t->arg);
        free(t);

        pthread_mutex_lock(&p->mtx);
        p->active--;
        // Wake up tpool_destroy if the pool is now fully idle
        if (p->qlen == 0 && p->active == 0)
            pthread_cond_broadcast(&p->drained);
        pthread_mutex_unlock(&p->mtx);
    }
    return NULL;
}


tpool_t *tpool_create(size_t nthreads){
    if (nthreads == 0) return NULL;

    tpool_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->workers = calloc(nthreads, sizeof(pthread_t));
    if (!p->workers) {
        free(p);
        return NULL;
    }

    p->nworkers = nthreads;
    pthread_mutex_init(&p->mtx, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->drained, NULL);

    // Spawn workers one by one; roll back on failure
    for (size_t i = 0; i < nthreads; i++) {
        if (pthread_create(&p->workers[i], NULL, worker_loop, p) != 0){
            // Signal stop to the workers already running, then join them
            pthread_mutex_lock(&p->mtx);
            p->stop = 1;
            pthread_cond_broadcast(&p->not_empty);
            pthread_mutex_unlock(&p->mtx);

            for (size_t j = 0; j < i; j++)
                pthread_join(p->workers[j], NULL);

            pthread_mutex_destroy(&p->mtx);
            pthread_cond_destroy(&p->not_empty);
            pthread_cond_destroy(&p->drained);
            free(p->workers);
            free(p);
            return NULL;
        }
    }
    return p;
}


int tpool_submit(tpool_t *p, tpool_task_fn fn, void *arg){
    if (!p || !fn) return -1;

    task_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;

    pthread_mutex_lock(&p->mtx);
    // Refuse new work once the pool is stopping
    if (p->stop) {
        pthread_mutex_unlock(&p->mtx);
        free(t);
        return -1;
    }

    // Append to the FIFO
    // Append to the FIFO
    if (p->tail) p->tail->next = t;
    else p->head = t;

    p->tail = t;
    p->qlen++;

    // Signal one waiting worker (broadcast would be wasteful here)
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->mtx);
    return 0;
}


void tpool_destroy(tpool_t *p){
    if (!p) return;

    // Wait until every queued task has been consumed AND every worker
    // currently running a task is done.
    pthread_mutex_lock(&p->mtx);
    while (p->qlen > 0 || p->active > 0)
        pthread_cond_wait(&p->drained, &p->mtx);

    // Order every worker to exit. Broadcast (not signal) so all wake up.
    p->stop = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->mtx);

    for (size_t i = 0; i < p->nworkers; i++)
        pthread_join(p->workers[i], NULL);

    pthread_mutex_destroy(&p->mtx);
    pthread_cond_destroy(&p->not_empty);
    pthread_cond_destroy(&p->drained);
    free(p->workers);
    free(p);
}

