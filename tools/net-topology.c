/**
 * @file net-topology.c
 * @brief Network interface enumeration with IRQ affinity reporting.
 * @details Enumerates every network interface (MAC, MTU, state, speed),
 *          lists IPv4 addresses, parses /proc/net/route for default
 *          routes, dumps the ARP table and reports IRQ-to-CPU affinity
 *          for each NIC-attached interrupt.
 */

// Required for getifaddrs and extended ifaddrs features
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * @brief Reads the first line of a file, trimming trailing whitespace.
 * @param path   File to read.
 * @param buf    Destination buffer.
 * @param buflen Buffer size in bytes.
 * @return 0 on success, -1 on failure.
 */
static int read_line(const char *path, char *buf, size_t buflen){
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    if(!fgets(buf, (int)buflen, f)){
        fclose(f);
        return -1;
    }
    fclose(f);

    // Strip trailing \n, space and tab characters
    size_t n = strlen(buf);
    while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ' || buf[n-1] == '\t')){
        buf[--n] = 0;
    }
    return 0;
}


/**
 * @brief Lists every network interface from /sys/class/net.
 * @details For each interface prints MAC, MTU, operational state and
 *          advertised link speed. Values come straight from sysfs.
 */
static void section_interfaces(void){
    printf("\n=== Network interfaces ===\n");
    printf("%-12s %-19s %-7s %-8s %-10s\n", "Iface", "MAC", "MTU", "State", "Speed");

    DIR *d = opendir("/sys/class/net");
    if (!d) { perror("opendir /sys/class/net"); return; }

    struct dirent *e;
    while((e = readdir(d))){
        // Skip hidden entries including "." and ".."
        if(e->d_name[0] == '.') continue;

        char path[512], mac[32] = "?", mtu[16] = "?", state[16] = "?", speed[16] = "-";

        // Read MAC address
        snprintf(path, sizeof(path), "sys/class/net/%.64s/address", e->d_name);
        read_line(path, mac, sizeof(mac));

        // Read MTU
        snprintf(path, sizeof(path), "/sys/class/net/%.64s/mtu", e->d_name);
        read_line(path, mtu, sizeof(mtu));

        // Read operational state (up / down / unknown)
        snprintf(path, sizeof(path), "/sys/class/net/%.64s/operstate", e->d_name);
        read_line(path, state, sizeof(state));

        // Read link speed (may fail for virtual interfaces)
        snprintf(path, sizeof(path), "/sys/class/net/%.64s/speed", e->d_name);
        read_line(path, speed, sizeof(speed));

        printf("%-12s %-19s %-7s %-8s %-10s\n", e->d_name, mac, mtu, state, speed);
    }
    closedir(d);
}


/**
 * @brief Lists IPv4 addresses per interface via getifaddrs().
 * @details getifaddrs() is the portable way to walk the system's
 *          interface list; we filter on AF_INET to keep the output tight.
 */
static void section_ipv4(void){
    printf("\n=== IPv4 addresses ===\n");

    struct ifaddrs *head = NULL;
    if(getifaddrs(&head) != 0){
        perror("getifaddrs"); return;
    }

    for (struct ifaddrs *ifa = head; ifa != NULL; ifa = ifa->ifa_next){
        // Skip anything that isn't an IPv4 socket address
        if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;

        char ip[INET_ADDRSTRLEN] = "?", mask[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, sizeof(ip));
        if(ifa->ifa_netmask){
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr, mask, sizeof(mask));
        }
        printf("  %-12s %s/%s\n", ifa->ifa_name, ip, mask);
    }
    freeifaddrs(head);
}


/**
 * @brief Parses /proc/net/route and prints each route.
 * @details Each field in /proc/net/route is a hex integer. We decode
 *          destination, gateway, metric and the owning interface.
 */
static void section_routes(void){
    printf("\n=== Default routes (/proc/net/route) ===\n");

    FILE *f = fopen("/proc/net/route", "r");
    if (!f) { perror("open /proc/net/route"); return; }

    char line[512];
    // Discard the header line
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    printf("%-10s %-16s %-16s %-8s\n", "Iface", "Destination", "Gateway", "Metric");
    while(fgets(line, sizeof(line), f)){
        char iface[16];
        unsigned dest, gw, flags, refc, use, metric, mask;

        // Columns are hex except reference counts, use, metric (decimal)
        if(sscanf(line, "%15s %x %x %x %u %u %u %x", iface, &dest, &gw, &flags, &refc, &use, &metric, &mask) < 8) continue;

        struct in_addr da = {.s_addr = dest}, ga = {.s_addr = gw};
        char ds[32], gs[32];
        snprintf(ds, sizeof(ds), "%s", inet_ntoa(da));
        snprintf(gs, sizeof(gs), "%s", inet_ntoa(ga));

        printf("%-10s %-16s %-16s %-8u\n", iface, ds, gs, metric);
    }
    fclose(f);
}


/**
 * @brief Dumps the ARP cache from /proc/net/arp.
 */
static void section_arp(void){
    printf("\n=== ARP table ===\n");

    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) { perror("open /proc/net/arp"); return; }

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    printf("%-16s %-19s %-8s %-12s\n", "IP", "MAC", "Flags", "Device");
    while(fgets(line, sizeof(line), f)){
        char ip[32], hwt[16], flags[16], mac[32], mask[32], dev[16];
        if(sscanf(line, "%31s %15s %15s %31s %31s %15s", ip, hwt, flags, mac, mask, dev) < 6){
            continue;
        }
        printf("%-16s %-19s %-8s %-12s\n", ip, mac, flags, dev);
    }
    fclose(f);
}


/**
 * @brief Reports the CPU affinity of each NIC-attached IRQ.
 * @details Scans /proc/interrupts; for every IRQ whose last column is a
 *          network device name, prints the smp_affinity_list taken from
 *          /proc/irq/N/smp_affinity_list. This is the field that decides
 *          whether packet processing lands on the right CPU.
 */
static void section_irq(void){
    printf("\n=== NIC IRQ affinity ===\n");

    FILE *f = fopen("/proc/interrupts", "r");
    if (!f) { perror("open /proc/interrupts"); return; }

    printf("%-8s %-14s %-16s\n", "IRQ", "Iface hint", "smp_affinity");

    char line[1024];
    // First line is the CPU header; skip it
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        // Advance past leading whitespace
        char *p = line;
        while(*p == ' ') p++;
        if(*p < '0' || *p > '9') continue;

        int irq = atoi(p);

        // Strip trailing whitespace
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == ' ')){
            line[--l] = 0;
        }

        // The last whitespace-separated token is the handler name
        char *last = strrchr(line, ' ');
        const char *tail = last ? last + 1 : line;

        // Keep only NIC-like names: eth, en, wl, ib
        if (!(strncmp(tail, "eth", 3) == 0 || strncmp(tail, "en", 2) == 0 ||
                strncmp(tail, "wl", 2) == 0  || strncmp(tail, "ib", 2) == 0)) continue;

        char apath[64], aff[64] = "?";
        snprintf(apath, sizeof(apath), "/proc/irq/%d/smp_affinity_list", irq);
        read_line(apath, aff, sizeof(aff));

        printf("%-8d %-14s %-16s\n", irq, tail, aff);
    }
    fclose(f);
}


/**
 * @brief Program entry point. Prints each section in sequence.
 */
int main(void){
    section_interfaces();
    section_ipv4();
    section_routes();
    section_arp();
    section_irq();

    printf("\nDone.\n");
    return 0;
}
