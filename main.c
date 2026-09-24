#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

#define PROC_NET_DEV "/proc/net/dev"
#define BUFFER_SIZE 512

// ==========================================
// Total Network Interface Stats
// ==========================================
typedef struct {
    char name[32];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} NetInterface;

bool get_active_interface_stats(NetInterface *iface) {
    FILE *file = fopen(PROC_NET_DEV, "r");
    if (!file) {
        return false;
    }

    char line[BUFFER_SIZE];
    bool found = false;
    unsigned long long max_traffic = 0;

    if (fgets(line, sizeof(line), file) == NULL || fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }

    while (fgets(line, sizeof(line), file)) {
        char if_name[32];
        unsigned long long rx = 0, tx = 0;
        unsigned long long dummy;

        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }

        *colon = '\0';
        sscanf(line, "%s", if_name);

        if (strcmp(if_name, "lo") == 0) {
            continue;
        }

        int parsed = sscanf(colon + 1,
                            "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
                            &tx);

        if (parsed >= 9) {
            unsigned long long total = rx + tx;
            if (total >= max_traffic) {
                max_traffic = total;
                strncpy(iface->name, if_name, sizeof(iface->name) - 1);
                iface->name[sizeof(iface->name) - 1] = '\0';
                iface->rx_bytes = rx;
                iface->tx_bytes = tx;
                found = true;
            }
        }
    }

    fclose(file);
    return found;
}

// ==========================================
// Per-Process Network Tracking
// ==========================================
typedef enum {
    PROTO_TCP,
    PROTO_UDP
} SocketProto;

typedef struct {
    unsigned long inode;
    unsigned long tx_queue;
    unsigned long rx_queue;
    SocketProto proto;
    pid_t pid;
    char comm[64];
} SocketEntry;

typedef struct {
    SocketEntry *entries;
    size_t count;
    size_t capacity;
} SocketList;

void init_socket_list(SocketList *list) {
    list->count = 0;
    list->capacity = 128;
    list->entries = malloc(list->capacity * sizeof(SocketEntry));
}

void add_socket(SocketList *list, unsigned long inode, unsigned long tx_q, unsigned long rx_q, SocketProto proto) {
    if (!list->entries) return;

    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity * 2;
        SocketEntry *new_entries = realloc(list->entries, new_cap * sizeof(SocketEntry));
        if (!new_entries) return;
        list->entries = new_entries;
        list->capacity = new_cap;
    }

    list->entries[list->count].inode = inode;
    list->entries[list->count].tx_queue = tx_q;
    list->entries[list->count].rx_queue = rx_q;
    list->entries[list->count].proto = proto;
    list->entries[list->count].pid = 0;
    list->entries[list->count].comm[0] = '\0';
    list->count++;
}

void free_socket_list(SocketList *list) {
    if (list->entries) {
        free(list->entries);
        list->entries = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

/**
 * Read /proc/net/tcp, /proc/net/udp, /proc/net/tcp6, /proc/net/udp6
 */
void parse_proc_net_file(const char *path, SocketProto proto, SocketList *list) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[BUFFER_SIZE];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        unsigned long tx_q = 0, rx_q = 0, inode = 0;
        char local_addr[128], rem_addr[128];
        unsigned int state = 0;
        int dummy_d1 = 0, dummy_d2 = 0;
        unsigned long dummy_ul1 = 0, dummy_ul2 = 0, dummy_ul3 = 0;

        int num_matched = sscanf(line,
               "%*d: %127s %127s %x %lx:%lx %lx:%lx %lx %d %d %lu",
               local_addr, rem_addr, &state, &tx_q, &rx_q,
               &dummy_ul1, &dummy_ul2, &dummy_ul3,
               &dummy_d1, &dummy_d2, &inode);

        if (num_matched >= 11 && inode > 0) {
            add_socket(list, inode, tx_q, rx_q, proto);
        }
    }

    fclose(f);
}

/**
 * Get process name from /proc/[pid]/comm safely
 */
void get_process_comm(pid_t pid, char *dest, size_t max_len) {
    char comm_path[128];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);

    FILE *f = fopen(comm_path, "r");
    if (f) {
        if (fgets(dest, max_len, f)) {
            size_t len = strlen(dest);
            if (len > 0 && dest[len - 1] == '\n') {
                dest[len - 1] = '\0';
            }
        } else {
            strncpy(dest, "[unknown]", max_len - 1);
            dest[max_len - 1] = '\0';
        }
        fclose(f);
    } else {
        strncpy(dest, "[defunct]", max_len - 1);
        dest[max_len - 1] = '\0';
    }
}

/**
 * Scan /proc/[pid]/fd to map sockets to PIDs
 */
void scan_proc_fds(SocketList *list) {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return;

    struct dirent *proc_entry;
    while ((proc_entry = readdir(proc_dir)) != NULL) {
        if (!isdigit(proc_entry->d_name[0])) {
            continue;
        }

        pid_t pid = (pid_t)atoi(proc_entry->d_name);
        if (pid <= 0) continue;

        char fd_dir_path[256];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);

        DIR *fd_dir = opendir(fd_dir_path);
        if (!fd_dir) {
            continue;
        }

        char comm[64] = {0};
        bool comm_cached = false;

        struct dirent *fd_entry;
        while ((fd_entry = readdir(fd_dir)) != NULL) {
            if (fd_entry->d_name[0] == '.') continue;

            char link_path[512];
            snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%s", pid, fd_entry->d_name);

            char link_target[256];
            ssize_t len = readlink(link_path, link_target, sizeof(link_target) - 1);
            if (len <= 0) continue;
            link_target[len] = '\0';

            unsigned long inode = 0;
            if (sscanf(link_target, "socket:[%lu]", &inode) == 1 && inode > 0) {
                for (size_t i = 0; i < list->count; i++) {
                    if (list->entries[i].inode == inode) {
                        list->entries[i].pid = pid;
                        if (!comm_cached) {
                            get_process_comm(pid, comm, sizeof(comm));
                            comm_cached = true;
                        }
                        snprintf(list->entries[i].comm, sizeof(list->entries[i].comm), "%s", comm);
                    }
                }
            }
        }
        closedir(fd_dir);
    }
    closedir(proc_dir);
}

// ==========================================
// Aggregation per Process
// ==========================================
typedef struct {
    pid_t pid;
    char comm[64];
    int tcp_sockets;
    int udp_sockets;
    unsigned long long total_rx_queue;
    unsigned long long total_tx_queue;
} ProcessNetUsage;

int compare_proc_usage(const void *a, const void *b) {
    const ProcessNetUsage *p1 = (const ProcessNetUsage *)a;
    const ProcessNetUsage *p2 = (const ProcessNetUsage *)b;
    int total_sockets_1 = p1->tcp_sockets + p1->udp_sockets;
    int total_sockets_2 = p2->tcp_sockets + p2->udp_sockets;
    return total_sockets_2 - total_sockets_1;
}

void print_per_process_stats(const SocketList *list) {
    ProcessNetUsage procs[512];
    size_t proc_count = 0;

    for (size_t i = 0; i < list->count; i++) {
        if (list->entries[i].pid <= 0) continue;

        pid_t pid = list->entries[i].pid;
        int found_idx = -1;

        for (size_t j = 0; j < proc_count; j++) {
            if (procs[j].pid == pid) {
                found_idx = (int)j;
                break;
            }
        }

        if (found_idx == -1 && proc_count < 512) {
            found_idx = (int)proc_count;
            procs[found_idx].pid = pid;
            snprintf(procs[found_idx].comm, sizeof(procs[found_idx].comm), "%s", list->entries[i].comm);
            procs[found_idx].tcp_sockets = 0;
            procs[found_idx].udp_sockets = 0;
            procs[found_idx].total_rx_queue = 0;
            procs[found_idx].total_tx_queue = 0;
            proc_count++;
        }

        if (found_idx != -1) {
            if (list->entries[i].proto == PROTO_TCP) {
                procs[found_idx].tcp_sockets++;
            } else {
                procs[found_idx].udp_sockets++;
            }
            procs[found_idx].total_rx_queue += list->entries[i].rx_queue;
            procs[found_idx].total_tx_queue += list->entries[i].tx_queue;
        }
    }

    qsort(procs, proc_count, sizeof(ProcessNetUsage), compare_proc_usage);

    printf("\n%-8s %-22s %-12s %-12s %-15s\n", "PID", "APPLICATION", "TCP SOCKETS", "UDP SOCKETS", "QUEUED (RX/TX)");
    printf("-------------------------------------------------------------------------\n");

    size_t max_display = proc_count < 15 ? proc_count : 15;
    for (size_t i = 0; i < max_display; i++) {
        printf("%-8d %-22s %-12d %-12d %llu / %llu B\n",
               procs[i].pid,
               procs[i].comm,
               procs[i].tcp_sockets,
               procs[i].udp_sockets,
               procs[i].total_rx_queue,
               procs[i].total_tx_queue);
    }

    if (proc_count == 0) {
        printf("  No active socket associations found.\n");
    }
    printf("-------------------------------------------------------------------------\n");
}

int main(void) {
    NetInterface active_iface;

    printf("=========================================================================\n");
    printf("     Network Usage Monitor - Snapshot Report                             \n");
    printf("=========================================================================\n");

    // 1. Overall network interface stats
    if (get_active_interface_stats(&active_iface)) {
        double rx_mb = (double)active_iface.rx_bytes / (1024.0 * 1024.0);
        double tx_mb = (double)active_iface.tx_bytes / (1024.0 * 1024.0);
        double total_mb = rx_mb + tx_mb;

        printf(" Active Interface : %s\n", active_iface.name);
        printf(" Total Download   : %8.2f MB\n", rx_mb);
        printf(" Total Upload     : %8.2f MB\n", tx_mb);
        printf(" Total Traffic    : %8.2f MB\n", total_mb);
    } else {
        printf(" Active Interface : None detected\n");
    }

    // 2. Per-process network sockets
    SocketList sock_list;
    init_socket_list(&sock_list);

    parse_proc_net_file("/proc/net/tcp", PROTO_TCP, &sock_list);
    parse_proc_net_file("/proc/net/tcp6", PROTO_TCP, &sock_list);
    parse_proc_net_file("/proc/net/udp", PROTO_UDP, &sock_list);
    parse_proc_net_file("/proc/net/udp6", PROTO_UDP, &sock_list);

    scan_proc_fds(&sock_list);

    print_per_process_stats(&sock_list);

    free_socket_list(&sock_list);

    return 0;
}
