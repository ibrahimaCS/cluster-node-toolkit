/**
 * @file echo-server.c
 * @brief Multi-client TCP echo server built on epoll + thread pool.
 * @details Edge-triggered epoll with EPOLLONESHOT so two workers never
 *          fight over the same descriptor. Reads are drained inside the
 *          worker; the FD is rearmed on the way out. Reusable thread
 *          pool from thread-pool.{h,c} handles concurrency.
 *          Usage:   ./echo-server [-p port] [-t threads]
 *          Compile: gcc -Wall -Wextra -Wpedantic -std=c11 -O2 -D_GNU_SOURCE \
 *                       echo-server.c thread-pool.c -o echo-server -lpthread
 */

#include <asm-generic/socket.h>
#include <netinet/in.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include "thread-pool.h"

#define MAX_EVENTS 128
#define BUF_SIZE 4096

// Flipped to 1 by SIGINT/SIGTERM; the main loop exits cleanly
static volatile sig_atomic_t g_stop = 0;

/**
 * @brief Signal handler for graceful shutdown.
 */
static void on_sigint(int s) {
    (void)s;
    g_stop = 1;
}


/**
 * @brief Switches a file descriptor to non-blocking mode.
 * @return 0 on success, -1 on fcntl error.
 */
static int set_nonblock(int fd){
    int fl = fcntl(fd, F_GETFL, 0);
    if(fl < 0){
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/**
 * @brief Per-connection state passed through epoll_event.data.ptr.
 */
typedef struct {
    int fd;  /**< Client socket descriptor */
    int epfd;  /**< Owning epoll descriptor */
} conn_t;


/**
 * @brief Worker task: drain the socket, echo everything back, rearm.
 * @details With edge-triggered epoll we MUST drain until EAGAIN or we
 *          will miss the next event. On EOF or hard error we unregister and close.
 */
static void handle_io(void *arg){
    conn_t *c = arg;
    char buf[BUF_SIZE];

    for(;;){
        ssize_t n = read(c->fd, buf, sizeof(buf));
        if(n > 0){
            // Echo back; may require mutiple writes if the kernel is slow
            ssize_t off = 0;
            while(off < n){
                ssize_t w = write(c->fd, buf + off, (size_t)(n - off));
                if(w < 0){
                    if(errno == EAGAIN) continue;
                    goto close_conn;
                }
                off += w;
            }
        }
        else if (n == 0) {
            // Clean peer close
            goto close_conn;
        }
        else{
            // EAGAIN means we drained the socket, exit the loop
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
            goto close_conn;
        }
    }
    
    // Rearm the FD for the next EPOLLIN under EPOLLONESHOT
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLONESHOT,
        .data.ptr = c
    };
    if(epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0){
        goto close_conn;
    }

close_conn:
    epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}


/**
 * @brief Program entry point.
 * @details Builds the listening socket, creates the epoll fd and the
 *          thread pool, then loops on epoll_wait. New connections are
 *          added with EPOLLONESHOT; data-ready events dispatch to the pool.
 */
int main(int argc, char *argv[]){
    int    port     = 9000;
    size_t nthreads = 4;

    // --- Parse command-line arguments ---
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc) nthreads = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [-p port] [-t threads]\n", argv[0]);
            return 0;
        }
    }

    // --- Signal handling: clean shutdown + ignore SIGPIPE on peer close ---
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    // --- Create the listening socket ---
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if(lfd < 0){
        perror("socket");
        return 1;
    }

    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(lfd);
        return 1;
    }
    if (listen(lfd, 128) < 0) {
        perror("listen");
        close(lfd);
        return 1;
    }
    set_nonblock(lfd);

    // --- Create the epoll instance ---
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        close(lfd);
        return 1;
    }

    // Register the listening socket (level-triggered is fine here)
    struct epoll_event lev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev);

    // --- Spin up the worker pool -----
    tpool_t *pool = tpool_create(nthreads);
    if (!pool) {
        fprintf(stderr, "tpool_create\n");
        return 1;
    }

    printf("echo-server listening on 0.0.0.0:%d (%zu workers)\n", port, nthreads);

    struct epoll_event events[MAX_EVENTS];
    while(!g_stop){
        int n = epoll_wait(epfd, events, MAX_EVENTS, 500);
        if(n < 0){
            if(errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for(int i = 0; i < n; i++){
            //--- Accept path ---
            if(events[i].data.fd == lfd){
                for(;;){
                    struct sockaddr_in ca;
                    socklen_t cl = sizeof(ca);
                    int cfd = accept4(lfd, (struct sockaddr *)&ca, &cl, SOCK_NONBLOCK);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    conn_t *c = malloc(sizeof(*c));
                    if (!c) {
                        close(cfd);
                        continue;
                    }
                    c->fd = cfd;
                    c->epfd = epfd;
                    struct epoll_event ev = {
                        .events = EPOLLIN | EPOLLONESHOT,
                        .data.ptr = c
                    };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
                }
            }
            // --- Data path: hand off to the worker pool ---
            else{
                conn_t *c = events[i].data.ptr;
                tpool_submit(pool, handle_io, c);
            }
        }
    }

    printf("\nShutting down...\n");
    tpool_destroy(pool);
    close(lfd);
    close(epfd);
    return 0;
}

