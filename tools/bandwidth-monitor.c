/**
 * @file bandwidth-monitor.c
 * @brief Real-time per-NIC bandwidth monitor from /proc/net/dev.
 * @details Reads /proc/net/dev once per second, computes RX/TX bytes
 *          and packet deltas, and refreshes an ANSI-escaped display
 *          without any ncurses dependency. Serves as a minimalist
 *          replacement for iftop.
 *          Usage:   ./bandwidth-monitor [-i seconds] [iface]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

// Caps on tracked interfaces and name length
#define MAX_IF 64
#define NAME_LEN 32

/**
 * @brief Snapshot of a single interface's counters.
 */
struct ifstat{
    char name[NAME_LEN];
    unsigned long long rx_bytes;
    unsigned long long rx_pkts;
    unsigned long long tx_bytes;
    unsigned long long tx_pkts;
};

static volatile sig_atomic_t stop = 0;

/**
 * @brief SIGINT handler: raises the stop flag.
 */
static void on_sigint(int s){
    (void)s;
    stop = 1;
}

/**
 * @brief Reads one snapshot of /proc/net/dev into @p s.
 * @param s      Destination array of ifstat entries.
 * @param max    Capacity of @p s.
 * @param filter If non-NULL, only the interface with this name is kept.
 * @return Number of entries written, or -1 on open error.
 */
static int read_stats(struct ifstat s[], int max, const char *filter){
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) { perror("open /proc/net/dev"); return -1; }

    char line[512];
    // Skip the two header lines of /proc/net/dev
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }

    int n = 0;
    while(fgets(line, sizeof(line), f) && n < max){
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;

        // Trim leading whitespace from the interface name
        char *name = line;
        while (*name == ' ') name++;

        if(filter && strcmp(name, filter) != 0) continue;

        unsigned long long rxb, rxp, tmp, txb, txp;
        // /proc/net/dev layout: rx_bytes rx_pkts rx_errs rx_drop rx_fifo
        // rx_frame rx_compressed rx_multicast tx_bytes tx_pkts ...
        int m = sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                &rxb, &rxp, &tmp, &tmp, &tmp, &tmp, &tmp, &tmp, &txb, &txp);
        if(m < 10) continue;

        snprintf(s[n].name, NAME_LEN, "%.31s", name);
        s[n].rx_bytes = rxb;
        s[n].rx_pkts  = rxp;
        s[n].tx_bytes = txb;
        s[n].tx_pkts  = txp;
        n++;
    }
    fclose(f);
    return n;
}


/**
 * @brief Formats a bytes-per-second value with the best unit (B/s, KB/s, ...).
 * @param bps Bandwidth in bytes per second.
 * @param out Output buffer.
 * @param n   Size of @p out.
 */
static void human_rate(double bps, char *out, size_t n){
    const char *u = "B/s";
    double v = bps;
    if (v > 1024.0) { v /= 1024.0; u = "KB/s"; }
    if (v > 1024.0) { v /= 1024.0; u = "MB/s"; }
    if (v > 1024.0) { v /= 1024.0; u = "GB/s"; }
    snprintf(out, n, "%8.2f %s", v, u);
}


/**
 * @brief Program entry point.
 * @details Parses command-line options, takes an initial snapshot, then
 *          loops forever (until Ctrl-C) refreshing the ANSI display.
 */
int main(int argc, char *argv[]){
    int interval = 1;
    const char *filter = NULL;

    // --- Parse command-line arguments ---
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i+1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 1) interval = 1;
        } else if (argv[i][0] != '-') {
            filter = argv[i];
        } else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [-i seconds] [iface]\n", argv[0]);
            return 0;
        }
    }

    // Catch Ctrl-C to exit the main loop cleanly
    signal(SIGINT, on_sigint);

    // Two snapshots so we can compute a delta on every tick
    struct ifstat prev[MAX_IF], curr[MAX_IF];
    int np = read_stats(prev, MAX_IF, filter);
    if (np <= 0) { fprintf(stderr, "No matching interfaces.\n"); return 1; }

    // Clear the terminal and position the cursor at the top-left
    printf("\033[2J");
    fflush(stdout);

    while (!stop) {
        sleep((unsigned)interval);

        int nc = read_stats(curr, MAX_IF, filter);
        if (nc <= 0) break;

        // Jump cursor back to top; this gives a flicker-free refresh
        printf("\033[H");

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        printf("bandwidth-monitor  %02d:%02d:%02d  interval=%ds\n\n",
               tm->tm_hour, tm->tm_min, tm->tm_sec, interval);
        printf("%-12s %-14s %-14s %-10s %-10s\n",
               "Iface", "RX", "TX", "RX pkts/s", "TX pkts/s");

        for (int i = 0; i < nc; i++) {
            // Find the matching previous snapshot by name
            int pi = -1;
            for (int j = 0; j < np; j++)
                if (strcmp(curr[i].name, prev[j].name) == 0) { pi = j; break; }
            if (pi < 0) continue;

            double drx = (double)(curr[i].rx_bytes - prev[pi].rx_bytes) / interval;
            double dtx = (double)(curr[i].tx_bytes - prev[pi].tx_bytes) / interval;
            double prx = (double)(curr[i].rx_pkts  - prev[pi].rx_pkts) / interval;
            double ptx = (double)(curr[i].tx_pkts  - prev[pi].tx_pkts) / interval;

            char h1[32], h2[32];
            human_rate(drx, h1, sizeof(h1));
            human_rate(dtx, h2, sizeof(h2));

            printf("%-12s %-14s %-14s %10.0f %10.0f\n",
                    curr[i].name, h1, h2, prx, ptx);
        }

        printf("\n(Ctrl-C to stop)\033[K\n");
        fflush(stdout);

        // Current becomes previous for the next tick
        memcpy(prev, curr, sizeof(prev));
        np = nc;
    }

    printf("\nStopped.\n");
    return 0;
}


