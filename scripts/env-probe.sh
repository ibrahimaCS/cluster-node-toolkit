#!/usr/bin/env bash
# env-probe.sh - Probe the machine for build & runtime prerequisites.

set -uo pipefail
OK="\033[32m OK\033[0m"; WARN="\033[33mWARN\033[0m"; FAIL="\033[31mFAIL\033[0m"
# Initialize counters for the final summary at the end of the script.
ok=0; warn=0; fail=0

# Function: check_mandatory
# Purpose: Checks a required prerequisite. If this test fails, compilation will be blocked.
# Arguments:
#   $1 = The name of the test to display (e.g., "Linux kernel")
#   $2 = The command to evaluate (e.g., "test -r /proc/cpuinfo")
check_mandatory(){
    # eval executes the string in $2 as an actual command.
    if eval "$2" >/dev/null 2>&1; then
        printf "  [%b] %s\n" "$OK" "$1"
        ok=$((ok+1)) # Increment the success counter
    else
        printf "  [%b] %s\n" "$FAIL" "$1"
        fail=$((fail+1)) # Increment the critical error counter
    fi
}

# Function: check_optional
# Purpose: Checks an optional prerequisite (bonuses, optimizations, extra tools).
check_optional() {
    if eval "$2" >/dev/null 2>&1; then
        printf "  [%b] %s\n" "$OK" "$1"
        ok=$((ok+1))
    else
        # On failure, display [WARN]
        printf "  [%b] %s (optional)\n" "$WARN" "$1"
        warn=$((warn+1))
    fi
}

echo "==== Cluster Node Toolkit - Environment probe ===="
echo

# --- Section: System Information ---
echo "-- System --"
# displays the kernel release version.
printf "  Kernel   : %s\n" "$(uname -r)"
# displays the hardware architecture.
printf "  Arch     : %s\n" "$(uname -m)"
# displays the network name of the machine.
printf "  Hostname : %s\n" "$(hostname)"
echo

# --- Section: Mandatory Build Tools ---
echo "-- Mandatory toolchain --"
# Checks that the operating system is indeed a Linux kernel.
check_mandatory "Linux kernel"    "test \"$(uname -s)\" = Linux"
# Checks for the GCC compiler.
check_mandatory "gcc (C11)"       "gcc --version | head -1 | grep -E 'gcc|GCC'"
# Checks for the 'make' build automation tool.
check_mandatory "GNU make"        "make --version | head -1 | grep -i make"
# Looks for the libpthread library in the dynamic linker cache.
check_mandatory "pthread (link test)" "echo 'int main(){return 0;}' | gcc -xc - -lpthread -o /dev/null 2>/dev/null"
check_mandatory "mpicc (MPI C)"   "command -v mpicc"
check_mandatory "mpirun"          "command -v mpirun"
echo

# --- Section: Kernel Interfaces (Virtual Filesystems) ---
echo "-- Kernel surfaces --"
# verifies that the CPU info file exists and is readable.
check_mandatory "/proc"       "test -r /proc/cpuinfo"
# verifies that the directory containing network interfaces exists.
check_mandatory "/sys"        "test -d /sys/class/net"
# Verifies access to global network statistics.
check_mandatory "/proc/net/dev" "test -r /proc/net/dev"
echo

# --- Section: Optional Tools and Configurations ---
echo "-- Optional --"
# Checks for the presence of a bash script linter/analyzer.
check_optional "shellcheck"  "command -v shellcheck"
# Checks if the system has allocated "HugePages".
check_optional "hugepages"  "grep -qE 'HugePages_Total: *[1-9]' /proc/meminfo"
# Checks if the script is being run as root (UID 0), which is required for raw network sockets.
check_optional "CAP_NET_RAW"  "test \"$(id -u)\" = 0"
echo

# --- Section: Network Interface Detection ---
echo "-- Detected network interfaces --"
# Loops through all subdirectories representing a network card in sysfs.
for iface in /sys/class/net/*/; do
    name=$(basename "$iface")
    # Reads the operational state of the card and catches errors if the file is unreadable and returns "?".
    state=$(cat "$iface/operstate" 2>/dev/null || echo "?")
    printf "  %-10s state=%s\n" "$name" "$state"
done
echo

# --- Section: Final Summary and Exit Code ---
echo "==== Summary: $ok OK, $warn WARN, $fail FAIL ===="

if [ "$fail" -gt 0 ]; then
    echo "Mandatory items missing. Fix before running make."
    # Exit with error code 1. This signals to the OS that the environment is not ready.
    exit 1
fi

exit 0
