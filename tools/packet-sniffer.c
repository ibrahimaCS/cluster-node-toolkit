/**
 * @file packet-sniffer.c
 * @brief Minimal L2/L3/L4 packet decoder over AF_PACKET.
 * @details Opens an AF_PACKET SOCK_RAW socket, optionally binds to one
 *          interface, and decodes each incoming frame. Supports Ethernet
 *          + IPv4 with TCP, UDP and ICMP payloads. No libpcap, no
 *          tcpdump. Requires CAP_NET_RAW (sudo or setcap).
 *          Usage:   sudo ./packet-sniffer [-n count] [iface]
 */

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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>

// Flipped to 1 by SIGINT; the capture loop exits cleanly
static volatile sig_atomic_t stop = 0;

/**
 * @brief SIGINT handler.
 */
static void on_sigint(int s) {
    (void)s;
    stop = 1;
}

/**
 * @brief Prints a 6-byte MAC address in colon-separated form.
 */
static void print_mac(const unsigned char m[6]) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}


/**
 * @brief Decodes a TCP header: source/destination ports and flag letters.
 * @param p   Pointer to the TCP header start.
 * @param len Remaining bytes available after @p p.
 */
static void decode_tcp(const unsigned char *p, size_t len){
    if(len < sizeof(struct tcphdr)){
        printf(" TCP[truncated]");
        return;
    }
    const struct tcphdr *t = (const struct tcphdr *)p;

    printf(" TCP %u -> %u flags=", ntohs(t->source), ntohs(t->dest));
    // Letter per flag: S=SYN, A=ACK, F=FIN, R=RST, P=PSH
    if(t->syn) putchar('S');
    if(t->ack) putchar('A');
    if(t->fin) putchar('F');
    if(t->rst) putchar('R');
    if(t->psh) putchar('P');
}


/**
 * @brief Decodes a UDP header: source/destination ports and length.
 */
static void decode_udp(const unsigned char *p, size_t len){
    if (len < sizeof(struct udphdr)){
        printf(" UDP[truncated]");
        return;
    }
    const struct udphdr *u = (const struct udphdr *)p;

    printf(" UDP %u -> %u len=%u", ntohs(u->source), ntohs(u->dest), ntohs(u->len));
}


/**
 * @brief Decodes an ICMP header: type and code.
 */
static void decode_icmp(const unsigned char *p, size_t len){
    if (len < sizeof(struct icmphdr)) {
        printf(" ICMP[truncated]");
        return;
    }
    const struct icmphdr *ic = (const struct icmphdr *)p;
    
    printf(" ICMP type=%u code=%u", ic->type, ic->code);
}


/**
 * @brief Decodes an IPv4 header and dispatches on the transport protocol.
 */
static void decode_ip(const unsigned char *p, size_t len){
    if (len < sizeof(struct iphdr)) {
        printf(" IP[truncated]");
        return;
    }
    const struct iphdr *ip = (const struct iphdr *)p;

    // IHL is in 32-bit words; multiply by 4 for the byte length
    size_t ihl = (size_t)ip->ihl * 4;
    if(ihl < sizeof(struct iphdr) || ihl > len){
        printf(" IP[bad ihl]");
        return;
    }

    char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip->saddr, src, sizeof(src));
    inet_ntop(AF_INET, &ip->daddr, dst, sizeof(dst));
    printf(" IPv4 %s -> %s ttl=%u", src, dst, ip->ttl);

    const unsigned char *l4 = p + ihl;
    size_t l4l = len - ihl;

    // Dispatch on the IP protocol field
    switch (ip->protocol) {
        case IPPROTO_TCP:  decode_tcp(l4, l4l);  break;
        case IPPROTO_UDP:  decode_udp(l4, l4l);  break;
        case IPPROTO_ICMP: decode_icmp(l4, l4l); break;
        default: printf(" proto=%u", ip->protocol);
    }
}


/**
 * @brief Top-level decoder: Ethernet header, then dispatch on EtherType.
 * @param buf Pointer to the raw frame as returned by recv().
 * @param len Length of the frame.
 */
static void decode_frame(const unsigned char *buf, ssize_t len){
    if (len < (ssize_t)sizeof(struct ether_header)) {
        printf("<truncated>\n");
        return;
    }
    const struct ether_header *eh = (const struct ether_header *)buf;

    printf("  src=");
    print_mac(eh->ether_shost);
    printf(" dst=");
    print_mac(eh->ether_dhost);

    unsigned short et = ntohs(eh->ether_type);
    printf(" type=0x%04x", et);

    if (et == ETHERTYPE_IP)
        decode_ip(buf + sizeof(struct ether_header),
                    (size_t)len - sizeof(struct ether_header));
    else if (et == ETHERTYPE_ARP) printf(" ARP");
    else printf(" (other)");

    putchar('\n');
}


/**
 * @brief Program entry point.
 * @details Opens a raw packet socket, optionally binds to one
 *          interface, and loops receiving + decoding until -n is reached
 *          or Ctrl-C is pressed.
 */
int main(int argc, char *argv[]){
    const char *ifname = NULL;
    long limit = -1;

    // --- Parse command-line arguments ---
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
            limit = strtol(argv[++i], NULL, 10);
        } else if (argv[i][0] != '-') {
            ifname = argv[i];
        } else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [-n count] [iface]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_sigint);

    // Open a raw socket seeing every protocol (ETH_P_ALL)
    int sfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sfd < 0) {
        // Clear hint if we are missing CAP_NET_RAW
        if (errno == EPERM || errno == EACCES) {
            fprintf(stderr,
                "socket(AF_PACKET, SOCK_RAW): %s\n"
                "Hint: needs CAP_NET_RAW; try sudo, or:\n"
                "  sudo setcap cap_net_raw+ep ./packet-sniffer\n",
                strerror(errno));
        } else {
            perror("socket");
        }
        return 1;
    }

    if (ifname) {
        if (setsockopt(sfd, SOL_SOCKET, SO_BINDTODEVICE,
                        ifname, (socklen_t)strlen(ifname)) < 0) {
            perror("SO_BINDTODEVICE");
            close(sfd);
            return 1;
        }
        printf("Sniffing on %s (Ctrl-C to stop)\n", ifname);
    } else {
        printf("Sniffing on all interfaces (Ctrl-C to stop)\n");
    }

    unsigned char buf[65536];
    long count = 0;

    while(!stop){
        ssize_t n = recv(sfd, buf, sizeof(buf), 0);
        if(n < 0){
            // EINTR is expected when Ctrl-C fires; just re-check stop
            if(errno == EINTR) continue;
            perror("recv");
            break;
        }
        printf("[%ld] %zd bytes\n", ++count, n);
        decode_frame(buf, n);
        if (limit > 0 && count >= limit) break;
    }

    close(sfd);
    printf("\nCaptured %ld frames.\n", count);
    return 0;
}

