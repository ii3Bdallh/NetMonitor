#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <getopt.h>
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

    // Optimize SQLite with WAL mode
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
// Network Interface & Sockets Collection
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

bool batch_insert_usage(Database *database, const ProcessNetUsage *procs, size_t count) {
    if (!database->db || !database->insert_stmt || count == 0) {
        return false;
    }

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

// Helper to format bytes to human readable string
void format_bytes(unsigned long long bytes, char *buffer, size_t buflen) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buffer, buflen, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buffer, buflen, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buffer, buflen, "%.2f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buffer, buflen, "%llu B", bytes);
    }
}

// Parse human size strings like "200M", "1G", "500K", "1024"
unsigned long long parse_size_string(const char *str) {
    if (!str) return 0;

    char *endptr = NULL;
    double val = strtod(str, &endptr);
    if (val < 0) val = 0;

    if (endptr && *endptr != '\0') {
        char unit = toupper((unsigned char)*endptr);
        if (unit == 'K') {
            return (unsigned long long)(val * 1024.0);
        } else if (unit == 'M') {
            return (unsigned long long)(val * 1024.0 * 1024.0);
        } else if (unit == 'G') {
            return (unsigned long long)(val * 1024.0 * 1024.0 * 1024.0);
        } else if (unit == 'T') {
            return (unsigned long long)(val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
        }
    }
    return (unsigned long long)val;
}

// ==========================================
// SQL Query & CLI Filter Handler (Stage 4)
// ==========================================
typedef struct {
    bool time_filter_active;
    char time_clause[128];
    unsigned long long min_bytes;
    bool sort_asc; // false = DESC (default), true = ASC
    bool run_daemon;
    bool record_snapshot;
} CliOptions;

void print_help(const char *prog_name) {
    printf("=========================================================================\n");
    printf("     Network Usage Monitor - Command Line Interface (CLI)                \n");
    printf("=========================================================================\n");
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Time Filtering Options (Executed in SQL):\n");
    printf("  -d, --last-day              Show usage records from the last 24 hours\n");
    printf("  -w, --last-week             Show usage records from the last 7 days\n");
    printf("  -m, --last-month            Show usage records from the last 30 days\n");
    printf("  -c, --custom \"YYYY-MM-DD\"   Filter records for a specific date\n\n");
    printf("Size Filtering & Sorting (Executed in SQL):\n");
    printf("      --min <SIZE>            Filter apps with traffic >= size (e.g., 200M, 1G, 500K)\n");
    printf("  -s, --sort <asc|desc>       Sort results by total consumption (default: desc)\n\n");
    printf("Recording / Execution Modes:\n");
    printf("      --record                Capture a live snapshot & record into database\n");
    printf("      --daemon                Run continuous background collector (writes every 60s)\n");
    printf("  -h, --help                  Display this help message and exit\n\n");
    printf("Examples:\n");
    printf("  %s --last-day --sort desc\n", prog_name);
    printf("  %s --last-week --min 200M\n", prog_name);
    printf("  %s --custom \"2026-09-24\" --sort asc\n", prog_name);
    printf("  %s --min 500K\n", prog_name);
    printf("=========================================================================\n");
}

void query_database(Database *database, const CliOptions *opts) {
    char sql[1024];
    char where_clause[512] = "";
    char having_clause[256] = "";

    // 1. Time Filter in SQL WHERE
    if (opts->time_filter_active && strlen(opts->time_clause) > 0) {
        snprintf(where_clause, sizeof(where_clause), "WHERE %s", opts->time_clause);
    }

    // 2. Min Size Filter in SQL HAVING (aggregated per application)
    if (opts->min_bytes > 0) {
        snprintf(having_clause, sizeof(having_clause), "HAVING total_bytes >= %llu", opts->min_bytes);
    }

    // 3. Sorting in SQL ORDER BY
    const char *order_dir = opts->sort_asc ? "ASC" : "DESC";

    // Build the complete SQL query
    snprintf(sql, sizeof(sql),
             "SELECT app_name, "
             "       SUM(bytes_sent) AS total_sent, "
             "       SUM(bytes_received) AS total_recv, "
             "       SUM(bytes_sent + bytes_received) AS total_bytes, "
             "       COUNT(*) AS entries_count, "
             "       MAX(timestamp) AS last_seen "
             "FROM network_usage "
             "%s "
             "GROUP BY app_name "
             "%s "
             "ORDER BY total_bytes %s, app_name ASC;",
             where_clause, having_clause, order_dir);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(database->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[SQLite Query Error] Failed to execute query: %s\n", sqlite3_errmsg(database->db));
        return;
    }

    printf("\n===================================================================================================\n");
    printf("                                   DATABASE QUERY RESULTS                                          \n");
    printf("===================================================================================================\n");
    printf(" Filter  : %s\n", opts->time_filter_active ? opts->time_clause : "All Recorded Time");
    if (opts->min_bytes > 0) {
        char min_str[32];
        format_bytes(opts->min_bytes, min_str, sizeof(min_str));
        printf(" Min Size: >= %s\n", min_str);
    }
    printf(" Sort    : Total Bytes %s\n", order_dir);
    printf("---------------------------------------------------------------------------------------------------\n");
    printf("%-24s %-16s %-16s %-16s %-10s %-20s\n",
           "APPLICATION", "SENT (TX)", "RECEIVED (RX)", "TOTAL USAGE", "SAMPLES", "LAST SEEN");
    printf("---------------------------------------------------------------------------------------------------\n");

    int row_count = 0;
    unsigned long long grand_total_sent = 0;
    unsigned long long grand_total_recv = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *app = sqlite3_column_text(stmt, 0);
        unsigned long long sent = (unsigned long long)sqlite3_column_int64(stmt, 1);
        unsigned long long recv = (unsigned long long)sqlite3_column_int64(stmt, 2);
        unsigned long long total = (unsigned long long)sqlite3_column_int64(stmt, 3);
        int samples = sqlite3_column_int(stmt, 4);
        const unsigned char *last_seen = sqlite3_column_text(stmt, 5);

        char sent_str[32], recv_str[32], total_str[32];
        format_bytes(sent, sent_str, sizeof(sent_str));
        format_bytes(recv, recv_str, sizeof(recv_str));
        format_bytes(total, total_str, sizeof(total_str));

        printf("%-24s %-16s %-16s %-16s %-10d %-20s\n",
               app ? (const char *)app : "[unknown]",
               sent_str,
               recv_str,
               total_str,
               samples,
               last_seen ? (const char *)last_seen : "-");

        grand_total_sent += sent;
        grand_total_recv += recv;
        row_count++;
    }

    if (row_count == 0) {
        printf("  No matching records found in database for the given criteria.\n");
    } else {
        printf("---------------------------------------------------------------------------------------------------\n");
        char total_sent_str[32], total_recv_str[32], grand_total_str[32];
        format_bytes(grand_total_sent, total_sent_str, sizeof(total_sent_str));
        format_bytes(grand_total_recv, total_recv_str, sizeof(total_recv_str));
        format_bytes(grand_total_sent + grand_total_recv, grand_total_str, sizeof(grand_total_str));

        printf("%-24s %-16s %-16s %-16s Total Rows: %d\n",
               "TOTAL AGGREGATED", total_sent_str, total_recv_str, grand_total_str, row_count);
    }
    printf("===================================================================================================\n\n");

    sqlite3_finalize(stmt);
}

void execute_collector(Database *db, bool daemon_mode) {
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

        time_t now = time(NULL);

        if (!daemon_mode || (now - last_batch_time >= BATCH_INTERVAL_SECONDS)) {
            if (batch_insert_usage(db, procs, proc_count)) {
                last_batch_time = now;
            }
        }

        if (!daemon_mode) {
            printf("=========================================================================\n");
            printf("     Network Usage Monitor - Snapshot Recorded to DB                    \n");
            printf("=========================================================================\n");
            if (has_iface) {
                double rx_mb = (double)active_iface.rx_bytes / (1024.0 * 1024.0);
                double tx_mb = (double)active_iface.tx_bytes / (1024.0 * 1024.0);
                printf(" Active Interface : %s\n", active_iface.name);
                printf(" Total Download   : %8.2f MB\n", rx_mb);
                printf(" Total Upload     : %8.2f MB\n", tx_mb);
                printf(" Total Traffic    : %8.2f MB\n", rx_mb + tx_mb);
            }
            printf(" [DB Status] Saved %d active application records into %s\n\n", proc_count, DB_FILE);
            free_socket_list(&sock_list);
            break;
        }

        free_socket_list(&sock_list);
        sleep(1);
    } while (keep_running);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    Database db = {0};
    if (!init_database(&db)) {
        fprintf(stderr, "Failed to initialize SQLite database. Exiting.\n");
        return 1;
    }

    CliOptions opts = {
        .time_filter_active = false,
        .time_clause = "",
        .min_bytes = 0,
        .sort_asc = false,
        .run_daemon = false,
        .record_snapshot = false
    };

    static struct option long_options[] = {
        {"last-day",   no_argument,       0, 'd'},
        {"last-week",  no_argument,       0, 'w'},
        {"last-month", no_argument,       0, 'm'},
        {"custom",     required_argument, 0, 'c'},
        {"min",        required_argument, 0, 'M'},
        {"sort",       required_argument, 0, 's'},
        {"record",     no_argument,       0, 'r'},
        {"daemon",     no_argument,       0, 'D'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    bool has_query_flags = false;
    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "dwmc:M:s:rDh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'd': // --last-day
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-1 day', 'localtime')");
                has_query_flags = true;
                break;

            case 'w': // --last-week
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-7 days', 'localtime')");
                has_query_flags = true;
                break;

            case 'm': // --last-month
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-30 days', 'localtime')");
                has_query_flags = true;
                break;

            case 'c': // --custom "YYYY-MM-DD"
                if (optarg) {
                    opts.time_filter_active = true;
                    snprintf(opts.time_clause, sizeof(opts.time_clause), "date(timestamp) = date('%s')", optarg);
                    has_query_flags = true;
                }
                break;

            case 'M': // --min <size>
                if (optarg) {
                    opts.min_bytes = parse_size_string(optarg);
                    has_query_flags = true;
                }
                break;

            case 's': // --sort asc|desc
                if (optarg) {
                    if (strcasecmp(optarg, "asc") == 0) {
                        opts.sort_asc = true;
                    } else if (strcasecmp(optarg, "desc") == 0) {
                        opts.sort_asc = false;
                    } else {
                        fprintf(stderr, "Invalid sort option '%s'. Use 'asc' or 'desc'.\n", optarg);
                        close_database(&db);
                        return 1;
                    }
                    has_query_flags = true;
                }
                break;

            case 'r': // --record
                opts.record_snapshot = true;
                break;

            case 'D': // --daemon
                opts.run_daemon = true;
                break;

            case 'h': // --help
                print_help(argv[0]);
                close_database(&db);
                return 0;

            default:
                print_help(argv[0]);
                close_database(&db);
                return 1;
        }
    }

    if (opts.run_daemon || opts.record_snapshot) {
        execute_collector(&db, opts.run_daemon);
    } else if (has_query_flags) {
        query_database(&db, &opts);
    } else {
        // Default behavior when no arguments given: record current snapshot and show query summary
        execute_collector(&db, false);
        query_database(&db, &opts);
    }

    close_database(&db);
    return 0;
}
