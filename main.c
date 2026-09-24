#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "sqlite3.h"

#define DB_FILE "usage.db"
#define PROC_NET_DEV "/proc/net/dev"
#define BUFFER_SIZE 512
#define BATCH_INTERVAL_SECONDS 60

static volatile bool keep_running = true;

void handle_sigint(int sig) {
    (void)sig;
    keep_running = false;
}

// ==========================================
// SQLite Database Layer
// ==========================================
typedef struct {
    sqlite3 *db;
    sqlite3_stmt *insert_stmt;
} Database;

bool init_database(Database *database) {
    int rc = sqlite3_open(DB_FILE, &database->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[SQLite Error] Cannot open database %s: %s\n", DB_FILE, sqlite3_errmsg(database->db));
        return false;
    }

    // Optimize SQLite for high-throughput, non-blocking operations (WAL Mode)
    sqlite3_exec(database->db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_exec(database->db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);

    const char *create_table_sql =
        "CREATE TABLE IF NOT EXISTS network_usage ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  app_name TEXT NOT NULL,"
        "  bytes_sent INTEGER NOT NULL DEFAULT 0,"
        "  bytes_received INTEGER NOT NULL DEFAULT 0,"
        "  timestamp DATETIME DEFAULT (datetime('now', 'localtime'))"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_app_time ON network_usage(app_name, timestamp);";

    char *err_msg = NULL;
    rc = sqlite3_exec(database->db, create_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[SQLite Error] Failed to create table: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(database->db);
        database->db = NULL;
        return false;
    }

    const char *insert_sql =
        "INSERT INTO network_usage (app_name, bytes_sent, bytes_received, timestamp) "
        "VALUES (?, ?, ?, datetime('now', 'localtime'));";

    rc = sqlite3_prepare_v2(database->db, insert_sql, -1, &database->insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[SQLite Error] Failed to prepare insert statement: %s\n", sqlite3_errmsg(database->db));
        sqlite3_close(database->db);
        database->db = NULL;
        return false;
    }

    return true;
}

void close_database(Database *database) {
    if (database->insert_stmt) {
        sqlite3_finalize(database->insert_stmt);
        database->insert_stmt = NULL;
    }
    if (database->db) {
        sqlite3_close(database->db);
        database->db = NULL;
    }
}

// ==========================================
// Network Interface & Sockets
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
                snprintf(iface->name, sizeof(iface->name), "%s", if_name);
                iface->rx_bytes = rx;
                iface->tx_bytes = tx;
                found = true;
            }
        }
    }

    fclose(file);
    return found;
}

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
            snprintf(dest, max_len, "[unknown]");
        }
        fclose(f);
    } else {
        snprintf(dest, max_len, "[defunct]");
    }
}

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
// Aggregation & Batch Insert
// ==========================================
typedef struct {
    pid_t pid;
    char comm[64];
    int tcp_sockets;
    int udp_sockets;
    unsigned long long total_rx_queue;
    unsigned long long total_tx_queue;
} ProcessNetUsage;

int aggregate_process_usage(const SocketList *list, ProcessNetUsage *procs, size_t max_procs) {
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

        if (found_idx == -1 && proc_count < max_procs) {
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

    return (int)proc_count;
}

/**
 * Perform an ultra-fast, atomic Batch Insert into SQLite using a Transaction
 */
bool batch_insert_usage(Database *database, const ProcessNetUsage *procs, size_t count) {
    if (!database->db || !database->insert_stmt || count == 0) {
        return false;
    }

    // Begin transaction for high speed batch write without disk locking delays
    sqlite3_exec(database->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    for (size_t i = 0; i < count; i++) {
        sqlite3_reset(database->insert_stmt);
        sqlite3_clear_bindings(database->insert_stmt);

        sqlite3_bind_text(database->insert_stmt, 1, procs[i].comm, -1, SQLITE_STATIC);
        sqlite3_bind_int64(database->insert_stmt, 2, (sqlite3_int64)procs[i].total_tx_queue);
        sqlite3_bind_int64(database->insert_stmt, 3, (sqlite3_int64)procs[i].total_rx_queue);

        sqlite3_step(database->insert_stmt);
    }

    sqlite3_exec(database->db, "COMMIT;", NULL, NULL, NULL);
    return true;
}

int compare_proc_usage(const void *a, const void *b) {
    const ProcessNetUsage *p1 = (const ProcessNetUsage *)a;
    const ProcessNetUsage *p2 = (const ProcessNetUsage *)b;
    int total_sockets_1 = p1->tcp_sockets + p1->udp_sockets;
    int total_sockets_2 = p2->tcp_sockets + p2->udp_sockets;
    return total_sockets_2 - total_sockets_1;
}

void print_snapshot_report(const NetInterface *iface, const ProcessNetUsage *procs, size_t proc_count) {
    printf("=========================================================================\n");
    printf("     Network Usage Monitor - Stage 3 (SQLite Database Enabled)          \n");
    printf("=========================================================================\n");

    if (iface) {
        double rx_mb = (double)iface->rx_bytes / (1024.0 * 1024.0);
        double tx_mb = (double)iface->tx_bytes / (1024.0 * 1024.0);
        double total_mb = rx_mb + tx_mb;

        printf(" Active Interface : %s\n", iface->name);
        printf(" Total Download   : %8.2f MB\n", rx_mb);
        printf(" Total Upload     : %8.2f MB\n", tx_mb);
        printf(" Total Traffic    : %8.2f MB\n", total_mb);
    }

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

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    Database db = {0};
    if (!init_database(&db)) {
        fprintf(stderr, "Failed to initialize SQLite database. Exiting.\n");
        return 1;
    }

    bool daemon_mode = (argc > 1 && strcmp(argv[1], "--daemon") == 0);

    time_t last_batch_time = 0;

    do {
        NetInterface active_iface;
        bool has_iface = get_active_interface_stats(&active_iface);

        SocketList sock_list;
        init_socket_list(&sock_list);

        parse_proc_net_file("/proc/net/tcp", PROTO_TCP, &sock_list);
        parse_proc_net_file("/proc/net/tcp6", PROTO_TCP, &sock_list);
        parse_proc_net_file("/proc/net/udp", PROTO_UDP, &sock_list);
        parse_proc_net_file("/proc/net/udp6", PROTO_UDP, &sock_list);

        scan_proc_fds(&sock_list);

        ProcessNetUsage procs[512];
        int proc_count = aggregate_process_usage(&sock_list, procs, 512);

        qsort(procs, proc_count, sizeof(ProcessNetUsage), compare_proc_usage);

        time_t now = time(NULL);

        // Always save batch in snapshot mode, or every BATCH_INTERVAL_SECONDS in daemon mode
        if (!daemon_mode || (now - last_batch_time >= BATCH_INTERVAL_SECONDS)) {
            if (batch_insert_usage(&db, procs, proc_count)) {
                last_batch_time = now;
            }
        }

        if (!daemon_mode) {
            print_snapshot_report(has_iface ? &active_iface : NULL, procs, proc_count);
            printf(" [DB Status] Successfully saved %d application records to %s\n\n", proc_count, DB_FILE);
            free_socket_list(&sock_list);
            break;
        }

        free_socket_list(&sock_list);
        sleep(1);
    } while (keep_running);

    close_database(&db);
    return 0;
}
