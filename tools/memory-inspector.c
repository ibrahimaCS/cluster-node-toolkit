/**
 * @file memory-inspector.c
 * @brief RAM, hugepages, TLB reach, NUMA meminfo inspector.
 * @details Compile: gcc -Wall -Wextra -Wpedantic -std=c11 -O2 memory-inspector.c -o memory-inspector
 */

// Request POSIX.1-2008 standard features from the GNU C Library
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/**
 * @brief Converts raw kilobytes into a human-readable string (KiB, MiB, GiB, TiB).
 * 
 * @param kb  The memory size in kilobytes.
 * @param out The character buffer where the human-readable string will be written.
 * @param n   The maximum size of the output buffer.
 */
static void human_kb(long kb, char *out, size_t n) {
    double v = (double)kb;
    const char *u = "KiB";

    // Iteratively divide by 1024 to find the most appropriate unit
    if(v > 1024.0) { v /= 1024.0; u = "MiB";}
    if (v > 1024.0) { v /= 1024.0; u = "GiB"; }
    if (v > 1024.0) { v /= 1024.0; u = "TiB"; }
    
    // Format the result to 2 decimal places
    snprintf(out, n, "%.2f %s", v, u);
}


/**
 * @brief Parses /proc/meminfo to find a specific key and returns its value.
 * @details The format in /proc/meminfo is typically: "Key:        123456 kB"
 * @param path The path to the file to parse (e.g., "/proc/meminfo").
 * @param key  The key string to search for.
 * @return long The numeric value associated with the specified key, or -1 if not found/error.
 */
static long meminfo_long(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return -1; // Return -1 if file cannot be opened
    
    char line[256];
    long val = -1;

    while (fgets(line, sizeof(line), f)) {
        size_t klen = strlen(key);
        // Check if the line starts with our target key followed by a colon
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            // Convert the numeric part to a long integer (skipping the key and colon)
            val = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return val;
}


/**
 * @brief Displays a summary of the global system memory (RAM).
 * @details Extracts and formats Total, Available, Free, Cached, and Swap memory.
 */
static void section_ram(void) {
    printf("\n=== System RAM ===\n");
    
    // Array of structs mapping /proc/meminfo keys to human-readable labels
    struct { const char *k; const char *label; } rows[] = {
        {"MemTotal",     "Total       "},
        {"MemAvailable", "Available   "},
        {"MemFree",      "Free        "},
        {"Cached",       "Cached      "},
        {"SwapTotal",    "Swap total  "},
        {"SwapFree",     "Swap free   "},
    };
    
    char h[32];
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        long kb = meminfo_long("/proc/meminfo", rows[i].k);
        if(kb < 0) continue;

        human_kb(kb, h, sizeof(h)); // Convert to human-readable format
        printf("  %s : %-14s (%ld kB)\n", rows[i].label, h, kb);
    }
}


/**
 * @brief Analyzes HugePages configuration.
 * @details HugePages reduce TLB pressure by allocating memory in larger chunks (e.g., 2MB or 1GB vs 4KB).
 *          This function lists the default size, total pages, free pages, and reserved memory.
 */
static void section_hugepages(void) {
    printf("\n=== Hugepages ===\n");
    
    // Read global HugePages statistics from /proc/meminfo
    long total = meminfo_long("/proc/meminfo", "HugePages_Total");
    long free_ = meminfo_long("/proc/meminfo", "HugePages_Free");
    long size  = meminfo_long("/proc/meminfo", "Hugepagesize");

    printf("  Default size    : %ld kB\n", size);
    printf("  Total pages     : %ld\n", total);
    printf("  Free pages      : %ld\n", free_);

    if(total > 0 && size > 0){
        char h[32];
        human_kb(total * size, h, sizeof(h)); // Calculate total RAM reserved for HugePages
        printf("  Total reserved  : %s\n", h);
    }

    // Scan sysfs to find all supportted hugepage sizes
    DIR *d = opendir("/sys/kernel/mm/hugepages");
    if(d){
        struct dirent *e;
        while((e = readdir(d))){
            // Look for directories starting with "hugepages-"
            if(strncmp(e->d_name, "hugepages-", 10) != 0) continue;

            char path[256], buf[32];
            long nr = 0;

            // Build path to read the number of pages allocated for this specific size
            snprintf(path, sizeof(path), "/sys/kernel/mm/hugepages/%.64s/nr_hugepages", e->d_name);

            FILE *f = fopen(path, "r");
            if(f){
                if(fgets(buf, sizeof(buf), f)){
                    nr = strtol(buf, NULL, 10);
                }
                fclose(f);
            }
            printf("  %s : %ld pages\n", e->d_name, nr);
        }
        closedir(d);
    }
}


/**
 * @brief Estimates the TLB (Translation Lookaside Buffer) reach.
 * @details This illustrates how much RAM can be accessed without a cache miss
 *          depending on the page size (Standard 4K vs Huge 2M), and compares it to total RAM.
 */
static void section_tlb_reach(void) {
    printf("\n=== TLB reach (estimate) ===\n");
    
    const long stlb_4k = 1536; // L2 STLB entries for 4KB pages
    const long dtlb_2m = 32;   // DTLB entries for 2MB pages

    long ram_kb = meminfo_long("/proc/meminfo", "MemTotal");
    char h[32];

    // Calculate total reach in kilobytes (entries * page_size_in_kb)
    long r4k = stlb_4k * 4;
    long r2m = dtlb_2m * 2048;

    human_kb(r4k, h, sizeof(h));
    printf("  Reach with 4K pages (STLB)  : %s\n", h);
    
    human_kb(r2m, h, sizeof(h));
    printf("  Reach with 2M pages         : %s\n", h);

    // Calculate the percentage of total RAM covered by the TLB
    if(ram_kb > 0){
        printf(" vs RAM: 4K=%.4f%%, 2M=%.4f%%\n",
        100.0 * (double)r4k / (double)ram_kb,
             100.0 * (double)r2m / (double)ram_kb);
    }
}


/**
 * @brief Maps out memory distributed across NUMA (Non-Uniform Memory Access) nodes.
 * @details Reads through /sys/devices/system/node to display memory metrics per NUMA node.
 */
static void section_numa(void) {
    printf("\n=== Per-NUMA-node memory ===\n");
    
    // Open the system node directory to discover NUMA nodes
    DIR *d = opendir("/sys/devices/system/node");
    if (!d) { printf("  (non-NUMA kernel)\n"); return; }
    
    struct dirent *e;
    int found = 0;

    while ((e = readdir(d))) {
        // Look for directories named "node0", "node1", etc.
        if (strncmp(e->d_name, "node", 4) != 0) continue;
        if (e->d_name[4] < '0' || e->d_name[4] > '9') continue;
        
        found = 1;
        char path[256];
        // Build path to the meminfo file specific to this NUMA node
        snprintf(path, sizeof(path), "/sys/devices/system/node/%.64s/meminfo", e->d_name);
        
        long total = 0, free_ = 0;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        
        char line[256];
        while(fgets(line, sizeof(line), f)){
            long v;
            char k[32];
            // Parse lines like "Node 0 MemTotal : ....kB"
            if(sscanf(line, "Node %*d %31[^:]: %ld", k, &v) == 2){
                if(strstr(k, "MemTotal")) total = v;
                else if(strstr(k, "MemFree")) free_ = v;
            }
        }
        fclose(f);
        
        char h1[32], h2[32];
        human_kb(total, h1, sizeof(h1));
        human_kb(free_, h2, sizeof(h2));
        printf("  %s : total=%s free=%s\n", e->d_name, h1, h2);
    }
    if (!found) printf("  (no NUMA nodes)\n");
    closedir(d);
}


int main(void) {
    section_ram();
    section_hugepages();
    section_tlb_reach();
    section_numa();
    
    printf("\nDone.\n");
    return 0;
}

