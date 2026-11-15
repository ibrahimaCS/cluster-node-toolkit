/**
 * @file cpu-topology.c
 * @brief Compact CPU/NUMA/cache topology reporter.
 * @details Walks /sys/devices/system/cpu/ and /proc/cpuinfo.
 *          Compile: gcc -Wall -Wextra -Wpedantic -std=c11 -O2 cpu-topology.c -o cpu-topology
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

// Define maximum limits for arrays and paths to prevent buffer overflows
#define MAX_CPUS 512
#define PATH_MAX_LEN 512

/**
 * @brief Opens a file, reads the first line into a buffer, and strips
 *        trailing newlines or spaces.
 * 
 * @param path   The path to the file to be read.
 * @param buf    The buffer where the read line will be stored.
 * @param buflen The maximum number of bytes to read into the buffer.
 * @return int   0 on success, -1 on failure.
 */
static int read_line(const char *path, char *buf, size_t buflen){
    FILE *f = fopen(path, "r");
    if(!f) return -1;

    // Read up to buflen characters from the file
    if(!fgets(buf, (int)buflen, f)){
        fclose(f);
        return -1;
    }
    fclose(f);

    // Strip trailing newlines ('\n') and spaces to ensure clean strings
    size_t n = strlen(buf);
    while(n > 0 && (buf[n-1] == '\n' || buf[n - 1] == ' ')){
        buf[--n] = 0;
    }
    return 0;
}

/**
 * @brief Reads a single integer value from a specified file path.
 * 
 * @param path The path to the file containing the integer.
 * @param out  Pointer to a long integer where the read value will be stored.
 * @return int 0 on success, -1 on failure.
 */
static int read_int(const char *path, long *out){
    char buf[64];
    if(read_line(path, buf, sizeof(buf)) < 0) return -1;

    // Convert the string to a long int
    *out = strtol(buf, NULL, 10);

    return 0;
}

/**
 * @brief Scans /sys/devices/system/cpu to find all logical CPUs,
 *        extracts their IDs, and sorts them in ascending order.
 * 
 * @param cpus Array to store the discovered logical CPU IDs.
 * @param max  The maximum number of CPUs to discover (size of the array).
 * @return int The total number of CPUs found, or -1 on error.
 */
static int discover_cpus(int cpus[], int max){
    int n = 0;

    //Open the system CPU directory
    DIR *d = opendir("/sys/devices/system/cpu");
    if(!d){
        perror("open /sys/devices/system/cpu");
        return -1;
    }

    struct dirent *ent;
    // Iterate through all entries in the directory
    while((ent = readdir(d)) && n < max){
        // Look only for directories that start with "cpu"
        if(strncmp(ent->d_name, "cpu", 3) != 0) continue;

        // Check if the characters following the cpu are digits
        char *p = ent->d_name + 3;
        if(*p < '0' || *p > '9') continue; // non-cpu directory

        // Convert the numeric part to an integer and store it
        cpus[n++] = atoi(p);
    }
    closedir(d);

    // Perform a simple insertion sort to order CPU IDs (0, 1, 2, 3...)
    for(int i = 1; i < n; i++){
        int tmp = cpus[i];
        int j = i - 1;
        while(j >= 0 && cpus[j] > tmp){
            cpus[j+1] = cpus[j];
            j--;
        }
        cpus[j+1] = tmp;
    }

    return n;
}

/**
 * @brief Prints a table mapping each logical CPU to its physical core,
 *        physical socket (package), and NUMA node.
 * 
 * @param cpus  Array of discovered logical CPU IDs.
 * @param ncpus The number of CPUs in the array.
 */
static void report_cpu_map(const int cpus[], int ncpus){
    printf("\n=== Logical CPU -> core / socket / NUMA ===\n");
    printf("%-6s %-10s %-10s %-8s\n", "CPU", "Core", "Socket", "NUMA");

    for(int i = 0; i < ncpus; i++){
        int cpu = cpus[i];
        char path[PATH_MAX_LEN];
        long core = -1, sock = -1;

        // Read physical core ID
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        read_int(path, &core);

        //Read physical socket (package) ID
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
        read_int(path, &sock);

        char node[16] = "-";
        char dpath[PATH_MAX_LEN];
        snprintf(dpath, sizeof(dpath), "/sys/devices/system/cpu/cpu%d", cpu);

        // Determine NUME node by scanning the individual CPU directory
        DIR *d = opendir(dpath);
        if (d){
            struct dirent *e;
            while((e = readdir(d))){
                if(strncmp(e->d_name, "node", 4) == 0 && e->d_name[4] >= '0' &&
                    e->d_name[4] <= '9'){
                        snprintf(node, sizeof(node), "%s", e->d_name + 4);
                        break;
                }
            }
            closedir(d);
        }

        printf("%-6d %-10ld %-10ld %-8s\n", cpu, core, sock, node);
    }
}

/**
 * @brief Probes the cache hierarchy (L1, L2, L3) for CPU 0 and prints
 *        size, type, coherency line size, and associativity.
 * @details Assuming symmetric architecture, CPU 0 represents the whole chip.
 */
static void report_caches(void){
    printf("\n=== Cache hierarchy (CPU 0) ===\n");
    printf("%-6s %-8s %-10s %-16s %-12s\n", "Level", "Type", "Size", "Line size (B)", "Ways");

    for(int idx = 0; idx < 8; idx++){
        char path[PATH_MAX_LEN], v[64];

        // Check if this cache index exists by verifying read access to the 'level' file
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/level", idx);
        if (access(path, R_OK) != 0) break; // Stop if index doesn't exist

        long level = -1, line = 0, ways = 0;
        read_int(path, &level);

        // Read cache type (Data, Instruction, Unified)
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/type", idx);
        read_line(path, v, sizeof(v));
        char type[16];
        snprintf(type, sizeof(type), "%.15s", v);

        // Read cache size (e.g., "32K", "512K", "32768K")
        char size[16] = "?";
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/size", idx);
        read_line(path, size, sizeof(size));

        // Read cache line size in bytes (typically 64 bytes)
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/coherency_line_size", idx);
        read_int(path, &line);

        // Read N-way set associativity
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/ways_of_associativity", idx);
        read_int(path, &ways);

        printf("L%-5ld %-8s %-10s %-16ld %-12ld\n", level, type, size, line, ways);
    }
}


/**
 * @brief Reads the CPU frequency scaling settings (Governor, Current Freq,
 *        Maximum Freq) for all discovered CPUs.
 * 
 * @param cpus  Array of discovered logical CPU IDs.
 * @param ncpus The number of CPUs in the array.
 */
static void report_cpufreq(const int cpus[], int ncpus){
    printf("\n=== cpufreq ===\n");
    printf("%-6s %-18s %-14s %-14s\n", "CPU", "Governor", "Cur kHz", "Max kHz");

    for(int i = 0; i < ncpus; i++){
        int cpu = cpus[i];
        char path[PATH_MAX_LEN], gov[32] = "n/a";
        long cur = 0, mx = 0;

        // Read scaling governor
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        read_line(path, gov, sizeof(gov));

        // Read current operating frequency (in kHz)
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        read_int(path, &cur);

        // Read maximum allowed frequency (in kHz)
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        read_int(path, &mx);
        
        printf("%-6d %-18s %-14ld %-14ld\n", cpu, gov, cur, mx);
    }
}


/**
 * @brief Entry point of the CPU topology reporter tool.
 * @details Ties all discovery and reporting functions together.
 * 
 * @return int 0 on successful execution, 1 if no online CPUs are found.
 */
int main(void){
    int cpus[MAX_CPUS];

    // Step 1: Find all online logical CPUs
    int n = discover_cpus(cpus, MAX_CPUS);
    if (n <= 0) { 
        fprintf(stderr, "No online CPUs.\n"); 
        return 1; 
    }
    printf("Total online logical CPUs: %d\n", n);
    
    // Step 2: Print CPU mapping (Hyperthreading/Cores/Sockets/NUMA)
    report_cpu_map(cpus, n);
    
    // Step 3: Print Cache architecture
    report_caches();
    
    // Step 4: Print CPU frequency and power management details
    report_cpufreq(cpus, n);
    
    printf("\nDone. This is what an MPI launcher sees when choosing bindings.\n");

    return 0;
}


