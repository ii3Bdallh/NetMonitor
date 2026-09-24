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
#include <syslog.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "sqlite3.h"

#define SYSTEM_DB_DIR  "/var/lib/netmon"
#define SYSTEM_DB_FILE "/var/lib/netmon/usage.db"
#define PROC_NET_DEV "/proc/net/dev"
#define BUFFER_SIZE 512
#define BATCH_INTERVAL_SECONDS 60
#define MAX_TRACKED_APPS 512
#define MAX_PIDS 4096

static volatile bool keep_running = true;
static bool is_daemon_mode = false;

void handle_signal(int sig) {
    (void)sig;
    keep_running = false;
}

void log_message(int priority, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (is_daemon_mode) {
        vsyslog(priority, format, args);
    } else {
        if (priority == LOG_ERR) {
            fprintf(stderr, "[ERROR] ");
            vfprintf(stderr, format, args);
            fprintf(stderr, "\n");
        } else {
            vprintf(format, args);
            printf("\n");
        }
    }
    va_end(args);
}

void get_database_path(char *dest, size_t maxlen, bool for_writing) {
    if (access(SYSTEM_DB_FILE, R_OK) == 0) {
        snprintf(dest, maxlen, "%s", SYSTEM_DB_FILE);
        return;
    }

    if (for_writing) {
        if (mkdir(SYSTEM_DB_DIR, 0755) == 0 || errno == EEXIST || access(SYSTEM_DB_DIR, W_OK) == 0) {
            chmod(SYSTEM_DB_DIR, 0755);
            snprintf(dest, maxlen, "%s", SYSTEM_DB_FILE);
            return;
        }
    }

    const char *home = getenv("HOME");
    if (home) {
        char user_dir[256];
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/netmon", home);
        char user_db[512];
        snprintf(user_db, sizeof(user_db), "%s/usage.db", user_dir);

        if (access(user_db, R_OK) == 0) {
            snprintf(dest, maxlen, "%s", user_db);
            return;
        }

        if (for_writing) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", user_dir);
            system(cmd);
            snprintf(dest, maxlen, "%s", user_db);
            return;
        }
    }

    snprintf(dest, maxlen, "%s", SYSTEM_DB_FILE);
}

void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        log_message(LOG_ERR, "First fork failed during daemonization.");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        log_message(LOG_ERR, "setsid failed during daemonization.");
        exit(EXIT_FAILURE);
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        log_message(LOG_ERR, "Second fork failed during daemonization.");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    umask(0);

    int dev_null = open("/dev/null", O_RDWR);
    if (dev_null != -1) {
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        if (dev_null > STDERR_FILENO) {
            close(dev_null);
        }
    }
}

// ==========================================
// SQLite Database Layer
// ==========================================
typedef struct {
    sqlite3 *db;
    sqlite3_stmt *insert_stmt;
    char db_path[512];
} Database;

bool init_database(Database *database, bool for_writing) {
    get_database_path(database->db_path, sizeof(database->db_path), for_writing);

    int open_flags = for_writing ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    int rc = sqlite3_open_v2(database->db_path, &database->db, open_flags, NULL);

    if (rc != SQLITE_OK) {
        rc = sqlite3_open(database->db_path, &database->db);
    }

    if (rc != SQLITE_OK) {
        log_message(LOG_ERR, "Cannot open database at '%s': %s", database->db_path, sqlite3_errmsg(database->db));
        return false;
    }

    if (for_writing) {
        sqlite3_exec(database->db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
        sqlite3_exec(database->db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);

        const char *create_table_sql =
            "CREATE TABLE IF NOT EXISTS network_usage ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  app_name TEXT NOT NULL,"
            "  bytes_sent INTEGER NOT NULL DEFAULT 0,"
            "  bytes_received INTEGER NOT NULL DEFAULT 0,"
            "  is_local INTEGER NOT NULL DEFAULT 0,"
            "  timestamp DATETIME DEFAULT (datetime('now', 'localtime'))"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_app_time ON network_usage(app_name, timestamp);"
            "CREATE INDEX IF NOT EXISTS idx_local_time ON network_usage(is_local, timestamp);";

        char *err_msg = NULL;
        rc = sqlite3_exec(database->db, create_table_sql, NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            log_message(LOG_ERR, "Failed to create database table: %s", err_msg);
            sqlite3_free(err_msg);
        }

        sqlite3_exec(database->db, "ALTER TABLE network_usage ADD COLUMN is_local INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);

        const char *insert_sql =
            "INSERT INTO network_usage (app_name, bytes_sent, bytes_received, is_local, timestamp) "
            "VALUES (?, ?, ?, ?, datetime('now', 'localtime'));";

        rc = sqlite3_prepare_v2(database->db, insert_sql, -1, &database->insert_stmt, NULL);
        if (rc != SQLITE_OK) {
            log_message(LOG_ERR, "Failed to prepare insert statement: %s", sqlite3_errmsg(database->db));
            sqlite3_close(database->db);
            database->db = NULL;
            return false;
        }

        chmod(database->db_path, 0664);
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

bool clear_database(Database *database) {
    if (!database->db) return false;
    char *err_msg = NULL;
    int rc = sqlite3_exec(database->db, "DELETE FROM network_usage; VACUUM;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Failed to clear database: %s\n", err_msg ? err_msg : "Unknown error");
        sqlite3_free(err_msg);
        return false;
    }
    printf("===================================================================================================\n");
    printf(" [✓] Successfully cleared all network usage history from database.\n");
    printf(" Database: %s\n", database->db_path);
    printf("===================================================================================================\n");
    return true;
}

// ==========================================
// Network Interface & Sockets Collection
// ==========================================
typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} TotalNetStats;

bool is_ignored_interface(const char *name) {
    if (!name) return true;
    if (strcmp(name, "lo") == 0) return true;
    // Exclude virtual docker bridges and veth endpoints to avoid double counting physical NICs
    if (strncmp(name, "veth", 4) == 0) return true;
    if (strncmp(name, "br-", 3) == 0) return true;
    if (strcmp(name, "docker0") == 0) return true;
    return false;
}

bool get_all_interfaces_stats(TotalNetStats *stats) {
    FILE *file = fopen(PROC_NET_DEV, "r");
    if (!file) {
        return false;
    }

    char line[BUFFER_SIZE];
    stats->rx_bytes = 0;
    stats->tx_bytes = 0;

    if (fgets(line, sizeof(line), file) == NULL || fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }

    bool found = false;
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

        if (is_ignored_interface(if_name)) {
            continue;
        }

        int parsed = sscanf(colon + 1,
                            "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
                            &tx);

        if (parsed >= 9) {
            stats->rx_bytes += rx;
            stats->tx_bytes += tx;
            found = true;
        }
    }

    fclose(file);
    return found;
}

// Check if an IPv4 or IPv6 address belongs to private/local network (RFC 1918, link-local, loopback, CGNAT)
bool is_hex_ip_local(const char *hex_ip) {
    if (!hex_ip) return false;
    size_t len = strlen(hex_ip);
    if (len < 8) return false;

    // IPv6 Address (32 hex characters in /proc/net/tcp6: 4 words of 32-bit little-endian on x86)
    if (len == 32) {
        unsigned char ip6[16];
        for (int w = 0; w < 4; w++) {
            unsigned int word = 0;
            char word_str[9] = {0};
            strncpy(word_str, hex_ip + (w * 8), 8);
            if (sscanf(word_str, "%x", &word) != 1) return false;
            // Reverse CPU endianness per 32-bit word to reconstruct 16-byte network order IPv6
            ip6[w * 4 + 0] = (word >> 0) & 0xFF;
            ip6[w * 4 + 1] = (word >> 8) & 0xFF;
            ip6[w * 4 + 2] = (word >> 16) & 0xFF;
            ip6[w * 4 + 3] = (word >> 24) & 0xFF;
        }

        // 1. Unspecified :: (all zeros)
        bool all_zero = true;
        for (int i = 0; i < 16; i++) {
            if (ip6[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) return true;

        // 2. Loopback ::1 (15 zeros + 0x01)
        bool is_loopback = true;
        for (int i = 0; i < 15; i++) {
            if (ip6[i] != 0) { is_loopback = false; break; }
        }
        if (is_loopback && ip6[15] == 1) return true;

        // 3. IPv4-mapped IPv6 (::ffff:w.x.y.z -> 10 zeros + 0xFF + 0xFF + 4 bytes IPv4)
        bool is_v4_mapped = true;
        for (int i = 0; i < 10; i++) {
            if (ip6[i] != 0) { is_v4_mapped = false; break; }
        }
        if (is_v4_mapped && ip6[10] == 0xFF && ip6[11] == 0xFF) {
            unsigned char b1 = ip6[12];
            unsigned char b2 = ip6[13];
            if (b1 == 127) return true; // Loopback
            if (b1 == 10) return true;  // 10.0.0.0/8
            if (b1 == 192 && b2 == 168) return true; // 192.168.0.0/16
            if (b1 == 172 && (b2 >= 16 && b2 <= 31)) return true; // 172.16.0.0/12
            if (b1 == 100 && (b2 >= 64 && b2 <= 127)) return true; // 100.64.0.0/10 (CGNAT / Tailscale)
            if (b1 == 169 && b2 == 254) return true; // 169.254.0.0/16 Link-Local
            if (b1 >= 224) return true; // Multicast
            if (b1 == 0 && b2 == 0) return true;
            return false;
        }

        // 4. fe80::/10 (Link-Local IPv6: 1111 1110 10xx xxxx)
        if (ip6[0] == 0xFE && (ip6[1] & 0xC0) == 0x80) return true;

        // 5. fc00::/7 (Unique Local Address - ULA private IPv6: fc00:: and fd00::)
        if ((ip6[0] & 0xFE) == 0xFC) return true;

        // 6. ff00::/8 (Multicast IPv6)
        if (ip6[0] == 0xFF) return true;

        return false;
    }

    // Standard 8-character IPv4 (little-endian 32-bit int in /proc/net/tcp)
    unsigned int raw_ip = 0;
    if (sscanf(hex_ip, "%x", &raw_ip) != 1) return false;

    unsigned char b1 = raw_ip & 0xFF;
    unsigned char b2 = (raw_ip >> 8) & 0xFF;

    if (b1 == 127) return true;
    if (b1 == 10) return true;
    if (b1 == 192 && b2 == 168) return true;
    if (b1 == 172 && (b2 >= 16 && b2 <= 31)) return true;
    if (b1 == 100 && (b2 >= 64 && b2 <= 127)) return true; // 100.64.0.0/10 (CGNAT / Tailscale)
    if (b1 == 169 && b2 == 254) return true;
    if (b1 >= 224) return true;
    if (b1 == 0 && b2 == 0) return true;

    return false;
}

typedef struct {
    unsigned long inode;
    bool is_established;
    bool is_local;
    unsigned int port;
    pid_t pid;
} SocketEntry;

typedef struct {
    SocketEntry *entries;
    size_t count;
    size_t capacity;
} SocketList;

void init_socket_list(SocketList *list) {
    list->count = 0;
    list->capacity = 256;
    list->entries = malloc(list->capacity * sizeof(SocketEntry));
}

void add_socket(SocketList *list, unsigned long inode, bool is_est, bool is_local, unsigned int port) {
    if (!list->entries) return;

    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity * 2;
        SocketEntry *new_entries = realloc(list->entries, new_cap * sizeof(SocketEntry));
        if (!new_entries) return;
        list->entries = new_entries;
        list->capacity = new_cap;
    }

    list->entries[list->count].inode = inode;
    list->entries[list->count].is_established = is_est;
    list->entries[list->count].is_local = is_local;
    list->entries[list->count].port = port;
    list->entries[list->count].pid = 0;
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

void parse_proc_net_file(const char *path, SocketList *list) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[BUFFER_SIZE];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        unsigned long inode = 0;
        char local_addr[128], rem_addr[128];
        unsigned int state = 0;
        unsigned long tx_q = 0, rx_q = 0;
        int dummy_d1 = 0, dummy_d2 = 0;
        unsigned long dummy_ul1 = 0, dummy_ul2 = 0, dummy_ul3 = 0;

        int num_matched = sscanf(line,
               "%*d: %127s %127s %x %lx:%lx %lx:%lx %lx %d %d %lu",
               local_addr, rem_addr, &state, &tx_q, &rx_q,
               &dummy_ul1, &dummy_ul2, &dummy_ul3,
               &dummy_d1, &dummy_d2, &inode);

        if (num_matched >= 11 && inode > 0) {
            char *colon = strchr(rem_addr, ':');
            unsigned int port = 0;
            if (colon) {
                *colon = '\0';
                sscanf(colon + 1, "%x", &port);
            }

            bool is_local = is_hex_ip_local(rem_addr);
            // Also check standard local KDE Connect port 1716 (0x06B4)
            if (port == 1716 || port == 5353) {
                is_local = true;
            }

            bool is_est = (state == 1);
            add_socket(list, inode, is_est, is_local, port);
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

bool get_process_io_bytes(pid_t pid, unsigned long long *rchar, unsigned long long *wchar) {
    char io_path[128];
    snprintf(io_path, sizeof(io_path), "/proc/%d/io", pid);

    FILE *f = fopen(io_path, "r");
    if (!f) return false;

    char line[128];
    *rchar = 0;
    *wchar = 0;
    int matched = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "rchar:", 6) == 0) {
            *rchar = strtoull(line + 6, NULL, 10);
            matched++;
        } else if (strncmp(line, "wchar:", 6) == 0) {
            *wchar = strtoull(line + 6, NULL, 10);
            matched++;
        }
        if (matched >= 2) break;
    }

    fclose(f);
    return (matched >= 2);
}

// ==========================================
// Precise Per-Process Tracking Engine
// ==========================================
typedef struct {
    pid_t pid;
    char comm[64];
    unsigned long long prev_rchar;
    unsigned long long prev_wchar;
    unsigned long long delta_rchar;
    unsigned long long delta_wchar;
    bool has_network_socket;
    bool has_active_stream;
    int local_sockets;
    int remote_sockets;
    bool seen;
} PidSnapshot;

typedef struct {
    char app_name[64];
    unsigned long long wan_upload;
    unsigned long long wan_download;
    unsigned long long lan_upload;
    unsigned long long lan_download;
} AppBandwidth;

typedef struct {
    AppBandwidth apps[MAX_TRACKED_APPS];
    size_t count;
    PidSnapshot pids[MAX_PIDS];
    size_t pid_count;
} BandwidthTracker;

void init_tracker(BandwidthTracker *tracker) {
    tracker->count = 0;
    tracker->pid_count = 0;
    memset(tracker->apps, 0, sizeof(tracker->apps));
    memset(tracker->pids, 0, sizeof(tracker->pids));
}

AppBandwidth* get_or_create_app(BandwidthTracker *tracker, const char *name) {
    for (size_t i = 0; i < tracker->count; i++) {
        if (strcmp(tracker->apps[i].app_name, name) == 0) {
            return &tracker->apps[i];
        }
    }
    if (tracker->count < MAX_TRACKED_APPS) {
        size_t idx = tracker->count++;
        snprintf(tracker->apps[idx].app_name, sizeof(tracker->apps[idx].app_name), "%s", name);
        tracker->apps[idx].wan_upload = 0;
        tracker->apps[idx].wan_download = 0;
        tracker->apps[idx].lan_upload = 0;
        tracker->apps[idx].lan_download = 0;
        return &tracker->apps[idx];
    }
    return NULL;
}

PidSnapshot* get_or_create_pid(BandwidthTracker *tracker, pid_t pid) {
    for (size_t i = 0; i < tracker->pid_count; i++) {
        if (tracker->pids[i].pid == pid) {
            return &tracker->pids[i];
        }
    }
    if (tracker->pid_count < MAX_PIDS) {
        size_t idx = tracker->pid_count++;
        tracker->pids[idx].pid = pid;
        get_process_comm(pid, tracker->pids[idx].comm, sizeof(tracker->pids[idx].comm));
        tracker->pids[idx].prev_rchar = 0;
        tracker->pids[idx].prev_wchar = 0;
        tracker->pids[idx].delta_rchar = 0;
        tracker->pids[idx].delta_wchar = 0;
        tracker->pids[idx].has_network_socket = false;
        tracker->pids[idx].has_active_stream = false;
        tracker->pids[idx].local_sockets = 0;
        tracker->pids[idx].remote_sockets = 0;
        tracker->pids[idx].seen = true;
        return &tracker->pids[idx];
    }
    return NULL;
}

// Check purely local discovery daemons (KDE Connect daemon, mDNS avahi)
bool is_known_local_app(const char *comm) {
    if (!comm) return false;
    if (strcasecmp(comm, "kdeconnectd") == 0 ||
        strcasecmp(comm, "avahi-daemon") == 0) {
        return true;
    }
    return false;
}

double get_process_local_ratio(const PidSnapshot *ps) {
    if (is_known_local_app(ps->comm)) return 1.0;
    int total = ps->local_sockets + ps->remote_sockets;
    if (total == 0) return 0.0;
    return (double)ps->local_sockets / (double)total;
}

void sample_process_activity(BandwidthTracker *tracker, const SocketList *sock_list) {
    for (size_t i = 0; i < tracker->pid_count; i++) {
        tracker->pids[i].seen = false;
        tracker->pids[i].has_network_socket = false;
        tracker->pids[i].has_active_stream = false;
        tracker->pids[i].local_sockets = 0;
        tracker->pids[i].remote_sockets = 0;
        tracker->pids[i].delta_rchar = 0;
        tracker->pids[i].delta_wchar = 0;
    }

    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return;

    struct dirent *proc_entry;
    while ((proc_entry = readdir(proc_dir)) != NULL) {
        if (!isdigit(proc_entry->d_name[0])) continue;

        pid_t pid = (pid_t)atoi(proc_entry->d_name);
        if (pid <= 0) continue;

        char fd_dir_path[256];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);

        DIR *fd_dir = opendir(fd_dir_path);
        if (!fd_dir) continue;

        bool has_net = false;
        bool has_active = false;
        int local_sockets = 0;
        int remote_sockets = 0;

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
                for (size_t s = 0; s < sock_list->count; s++) {
                    if (sock_list->entries[s].inode == inode) {
                        has_net = true;
                        if (sock_list->entries[s].is_established) {
                            has_active = true;
                        }
                        if (sock_list->entries[s].is_local) {
                            local_sockets++;
                        } else {
                            remote_sockets++;
                        }
                        break;
                    }
                }
            }
        }
        closedir(fd_dir);

        if (has_net) {
            PidSnapshot *ps = get_or_create_pid(tracker, pid);
            if (ps) {
                ps->seen = true;
                ps->has_network_socket = true;
                ps->has_active_stream = has_active;
                ps->local_sockets = local_sockets;
                ps->remote_sockets = remote_sockets;

                unsigned long long rchar = 0, wchar = 0;
                if (get_process_io_bytes(pid, &rchar, &wchar)) {
                    if (ps->prev_rchar > 0 && rchar >= ps->prev_rchar) {
                        ps->delta_rchar = rchar - ps->prev_rchar;
                    }
                    if (ps->prev_wchar > 0 && wchar >= ps->prev_wchar) {
                        ps->delta_wchar = wchar - ps->prev_wchar;
                    }
                    ps->prev_rchar = rchar;
                    ps->prev_wchar = wchar;
                }
            }
        }
    }
    closedir(proc_dir);
}

void attribute_bandwidth(BandwidthTracker *tracker, unsigned long long delta_rx, unsigned long long delta_tx) {
    if (delta_rx == 0 && delta_tx == 0) return;

    unsigned long long total_active_rchar = 0;
    unsigned long long total_active_wchar = 0;
    int active_stream_count = 0;

    for (size_t i = 0; i < tracker->pid_count; i++) {
        if (!tracker->pids[i].seen || !tracker->pids[i].has_network_socket) continue;

        if (tracker->pids[i].has_active_stream) {
            active_stream_count++;
        }
        total_active_rchar += tracker->pids[i].delta_rchar;
        total_active_wchar += tracker->pids[i].delta_wchar;
    }

    // 1. Distribute Download (RX)
    if (delta_rx > 0) {
        if (total_active_rchar > 0) {
            for (size_t i = 0; i < tracker->pid_count; i++) {
                if (!tracker->pids[i].seen || tracker->pids[i].delta_rchar == 0) continue;
                unsigned long long share_rx = (delta_rx * tracker->pids[i].delta_rchar) / total_active_rchar;
                if (share_rx > 0) {
                    AppBandwidth *app = get_or_create_app(tracker, tracker->pids[i].comm);
                    if (app) {
                        double local_ratio = get_process_local_ratio(&tracker->pids[i]);
                        unsigned long long lan_rx = (unsigned long long)(share_rx * local_ratio);
                        unsigned long long wan_rx = share_rx - lan_rx;
                        app->lan_download += lan_rx;
                        app->wan_download += wan_rx;
                    }
                }
            }
        } else if (active_stream_count > 0) {
            unsigned long long share_rx = delta_rx / active_stream_count;
            for (size_t i = 0; i < tracker->pid_count; i++) {
                if (tracker->pids[i].seen && tracker->pids[i].has_active_stream) {
                    AppBandwidth *app = get_or_create_app(tracker, tracker->pids[i].comm);
                    if (app) {
                        double local_ratio = get_process_local_ratio(&tracker->pids[i]);
                        unsigned long long lan_rx = (unsigned long long)(share_rx * local_ratio);
                        unsigned long long wan_rx = share_rx - lan_rx;
                        app->lan_download += lan_rx;
                        app->wan_download += wan_rx;
                    }
                }
            }
        } else {
            AppBandwidth *app = get_or_create_app(tracker, "system-network");
            if (app) app->wan_download += delta_rx;
        }
    }

    // 2. Distribute Upload (TX)
    if (delta_tx > 0) {
        if (total_active_wchar > 0) {
            for (size_t i = 0; i < tracker->pid_count; i++) {
                if (!tracker->pids[i].seen || tracker->pids[i].delta_wchar == 0) continue;
                unsigned long long share_tx = (delta_tx * tracker->pids[i].delta_wchar) / total_active_wchar;
                if (share_tx > 0) {
                    AppBandwidth *app = get_or_create_app(tracker, tracker->pids[i].comm);
                    if (app) {
                        double local_ratio = get_process_local_ratio(&tracker->pids[i]);
                        unsigned long long lan_tx = (unsigned long long)(share_tx * local_ratio);
                        unsigned long long wan_tx = share_tx - lan_tx;
                        app->lan_upload += lan_tx;
                        app->wan_upload += wan_tx;
                    }
                }
            }
        } else if (active_stream_count > 0) {
            unsigned long long share_tx = delta_tx / active_stream_count;
            for (size_t i = 0; i < tracker->pid_count; i++) {
                if (tracker->pids[i].seen && tracker->pids[i].has_active_stream) {
                    AppBandwidth *app = get_or_create_app(tracker, tracker->pids[i].comm);
                    if (app) {
                        double local_ratio = get_process_local_ratio(&tracker->pids[i]);
                        unsigned long long lan_tx = (unsigned long long)(share_tx * local_ratio);
                        unsigned long long wan_tx = share_tx - lan_tx;
                        app->lan_upload += lan_tx;
                        app->wan_upload += wan_tx;
                    }
                }
            }
        } else {
            AppBandwidth *app = get_or_create_app(tracker, "system-network");
            if (app) app->wan_upload += delta_tx;
        }
    }
}

bool flush_tracker_to_db(Database *database, BandwidthTracker *tracker) {
    if (!database->db || !database->insert_stmt) return false;

    bool has_records = false;
    for (size_t i = 0; i < tracker->count; i++) {
        if (tracker->apps[i].wan_download > 0 || tracker->apps[i].wan_upload > 0 ||
            tracker->apps[i].lan_download > 0 || tracker->apps[i].lan_upload > 0) {
            has_records = true;
            break;
        }
    }

    if (!has_records) return false;

    sqlite3_exec(database->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    for (size_t i = 0; i < tracker->count; i++) {
        if (tracker->apps[i].wan_download > 0 || tracker->apps[i].wan_upload > 0) {
            sqlite3_reset(database->insert_stmt);
            sqlite3_clear_bindings(database->insert_stmt);

            sqlite3_bind_text(database->insert_stmt, 1, tracker->apps[i].app_name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(database->insert_stmt, 2, (sqlite3_int64)tracker->apps[i].wan_upload);
            sqlite3_bind_int64(database->insert_stmt, 3, (sqlite3_int64)tracker->apps[i].wan_download);
            sqlite3_bind_int(database->insert_stmt, 4, 0);

            sqlite3_step(database->insert_stmt);

            tracker->apps[i].wan_upload = 0;
            tracker->apps[i].wan_download = 0;
        }

        if (tracker->apps[i].lan_download > 0 || tracker->apps[i].lan_upload > 0) {
            sqlite3_reset(database->insert_stmt);
            sqlite3_clear_bindings(database->insert_stmt);

            sqlite3_bind_text(database->insert_stmt, 1, tracker->apps[i].app_name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(database->insert_stmt, 2, (sqlite3_int64)tracker->apps[i].lan_upload);
            sqlite3_bind_int64(database->insert_stmt, 3, (sqlite3_int64)tracker->apps[i].lan_download);
            sqlite3_bind_int(database->insert_stmt, 4, 1);

            sqlite3_step(database->insert_stmt);

            tracker->apps[i].lan_upload = 0;
            tracker->apps[i].lan_download = 0;
        }
    }

    sqlite3_exec(database->db, "COMMIT;", NULL, NULL, NULL);
    return true;
}

void format_bytes(unsigned long long bytes, char *buffer, size_t buflen) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buffer, buflen, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buffer, buflen, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buffer, buflen, "%.2f KB", (double)bytes / (1024.0));
    } else {
        snprintf(buffer, buflen, "%llu B", bytes);
    }
}

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
// SQL Query & CLI Filter Handler
// ==========================================
typedef struct {
    bool time_filter_active;
    char time_clause[128];
    char time_description[128];
    unsigned long long min_bytes;
    bool sort_asc;
    bool run_daemon;
    bool record_snapshot;
    bool clear_history;
} CliOptions;

void print_help(const char *prog_name) {
    printf("=========================================================================\n");
    printf("     Network Usage Monitor - Command Line Interface (CLI)                \n");
    printf("=========================================================================\n");
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Time Filtering Options (Executed in SQL):\n");
    printf("  --last-minute [N]           Show usage from last N minutes (default N=1)\n");
    printf("  --last-hour [N]             Show usage from last N hours   (default N=1)\n");
    printf("  -d, --last-day [N]          Show usage from last N days    (default N=1)\n");
    printf("  -w, --last-week [N]         Show usage from last N weeks   (default N=1)\n");
    printf("  -m, --last-month [N]        Show usage from last N months  (default N=1)\n");
    printf("  -c, --custom \"YYYY-MM-DD\"   Filter records for a specific date\n\n");
    printf("Size Filtering & Sorting (Executed in SQL):\n");
    printf("      --min <SIZE>            Filter apps with traffic >= size (e.g., 200M, 1G, 500K)\n");
    printf("  -s, --sort <asc|desc>       Sort results by total consumption (default: desc)\n\n");
    printf("Database Management:\n");
    printf("      --clear, --reset        Clear / reset all historical records from database\n\n");
    printf("Daemon & Execution Modes:\n");
    printf("  -D, --daemon                Run as background daemon service (logs to syslog)\n");
    printf("  -h, --help                  Display this help message and exit\n\n");
    printf("Examples:\n");
    printf("  %s --last-minute 30\n", prog_name);
    printf("  %s --last-hour 9\n", prog_name);
    printf("  %s --last-day 5 --sort desc\n", prog_name);
    printf("  %s --last-week 4 --min 200M\n", prog_name);
    printf("  %s --custom \"2026-09-24\" --sort asc\n", prog_name);
    printf("  %s --clear\n", prog_name);
    printf("=========================================================================\n");
}

void query_database(Database *database, const CliOptions *opts) {
    char sql[1024];
    char where_clause[512] = "";
    char having_clause[256] = "";

    if (opts->time_filter_active && strlen(opts->time_clause) > 0) {
        snprintf(where_clause, sizeof(where_clause), "WHERE %s", opts->time_clause);
    }

    if (opts->min_bytes > 0) {
        snprintf(having_clause, sizeof(having_clause), "HAVING total_bytes >= %llu", opts->min_bytes);
    }

    const char *order_dir = opts->sort_asc ? "ASC" : "DESC";

    snprintf(sql,
             sizeof(sql),
             "SELECT app_name, "
             "       SUM(bytes_sent) AS total_upload, "
             "       SUM(bytes_received) AS total_download, "
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
        log_message(LOG_ERR, "Failed to execute database query: %s", sqlite3_errmsg(database->db));
        return;
    }

    printf("\n===================================================================================================\n");
    printf("                                   DATABASE QUERY RESULTS                                          \n");
    printf("===================================================================================================\n");
    printf(" Database: %s\n", database->db_path);
    printf(" Filter  : %s\n", opts->time_filter_active ? opts->time_description : "All Recorded Time");
    if (opts->min_bytes > 0) {
        char min_str[32];
        format_bytes(opts->min_bytes, min_str, sizeof(min_str));
        printf(" Min Size: >= %s\n", min_str);
    }
    printf(" Sort    : Total Bytes %s\n", order_dir);
    printf("---------------------------------------------------------------------------------------------------\n");
    printf("%-24s %-16s %-16s %-16s %-10s %-20s\n",
           "APPLICATION", "UPLOAD (TX)", "DOWNLOAD (RX)", "TOTAL USAGE", "SAMPLES", "LAST SEEN");
    printf("---------------------------------------------------------------------------------------------------\n");

    int row_count = 0;
    unsigned long long grand_total_upload = 0;
    unsigned long long grand_total_download = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *app = sqlite3_column_text(stmt, 0);
        unsigned long long upload = (unsigned long long)sqlite3_column_int64(stmt, 1);
        unsigned long long download = (unsigned long long)sqlite3_column_int64(stmt, 2);
        unsigned long long total = (unsigned long long)sqlite3_column_int64(stmt, 3);
        int samples = sqlite3_column_int(stmt, 4);
        const unsigned char *last_seen = sqlite3_column_text(stmt, 5);

        char upload_str[32], download_str[32], total_str[32];
        format_bytes(upload, upload_str, sizeof(upload_str));
        format_bytes(download, download_str, sizeof(download_str));
        format_bytes(total, total_str, sizeof(total_str));

        printf("%-24s %-16s %-16s %-16s %-10d %-20s\n",
               app ? (const char *)app : "[unknown]",
               upload_str,
               download_str,
               total_str,
               samples,
               last_seen ? (const char *)last_seen : "-");

        grand_total_upload += upload;
        grand_total_download += download;
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0) {
        printf("  No matching records found in database for the given criteria.\n");
        printf("===================================================================================================\n\n");
        return;
    }

    printf("---------------------------------------------------------------------------------------------------\n");
    char total_up_str[32], total_down_str[32], grand_total_str[32];
    format_bytes(grand_total_upload, total_up_str, sizeof(total_up_str));
    format_bytes(grand_total_download, total_down_str, sizeof(total_down_str));
    format_bytes(grand_total_upload + grand_total_download, grand_total_str, sizeof(grand_total_str));

    printf("%-24s %-16s %-16s %-16s Total Rows: %d\n",
           "TOTAL AGGREGATED", total_up_str, total_down_str, grand_total_str, row_count);
    printf("---------------------------------------------------------------------------------------------------\n");

    char split_sql[1024];
    snprintf(split_sql,
             sizeof(split_sql),
             "SELECT is_local, "
             "       SUM(bytes_sent) AS sum_sent, "
             "       SUM(bytes_received) AS sum_recv, "
             "       SUM(bytes_sent + bytes_received) AS sum_total "
             "FROM network_usage "
             "%s "
             "GROUP BY is_local;",
             where_clause);

    sqlite3_stmt *split_stmt = NULL;
    unsigned long long wan_up = 0, wan_down = 0, wan_tot = 0;
    unsigned long long lan_up = 0, lan_down = 0, lan_tot = 0;

    if (sqlite3_prepare_v2(database->db, split_sql, -1, &split_stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(split_stmt) == SQLITE_ROW) {
            int is_local = sqlite3_column_int(split_stmt, 0);
            unsigned long long s_up = (unsigned long long)sqlite3_column_int64(split_stmt, 1);
            unsigned long long s_down = (unsigned long long)sqlite3_column_int64(split_stmt, 2);
            unsigned long long s_tot = (unsigned long long)sqlite3_column_int64(split_stmt, 3);

            if (is_local == 1) {
                lan_up = s_up;
                lan_down = s_down;
                lan_tot = s_tot;
            } else {
                wan_up = s_up;
                wan_down = s_down;
                wan_tot = s_tot;
            }
        }
        sqlite3_finalize(split_stmt);
    }

    char wan_tot_str[32], wan_up_str[32], wan_down_str[32];
    char lan_tot_str[32], lan_up_str[32], lan_down_str[32];

    format_bytes(wan_tot, wan_tot_str, sizeof(wan_tot_str));
    format_bytes(wan_up, wan_up_str, sizeof(wan_up_str));
    format_bytes(wan_down, wan_down_str, sizeof(wan_down_str));

    format_bytes(lan_tot, lan_tot_str, sizeof(lan_tot_str));
    format_bytes(lan_up, lan_up_str, sizeof(lan_up_str));
    format_bytes(lan_down, lan_down_str, sizeof(lan_down_str));

    printf("  🌐 Internet / WAN (Quota Usage) : %-10s (Upload: %-9s | Download: %-9s)\n",
           wan_tot_str, wan_up_str, wan_down_str);
    printf("  🏠 Local / LAN    (Network Sharing): %-10s (Upload: %-9s | Download: %-9s)\n",
           lan_tot_str, lan_up_str, lan_down_str);

    printf("===================================================================================================\n\n");
}

void run_daemon_collector(Database *db) {
    log_message(LOG_INFO, "NetMonitor continuous background collector started (DB: %s).", db->db_path);

    BandwidthTracker tracker;
    init_tracker(&tracker);

    TotalNetStats prev_stats = {0};
    bool has_prev = get_all_interfaces_stats(&prev_stats);

    time_t last_flush_time = time(NULL);

    while (keep_running) {
        sleep(1);

        TotalNetStats curr_stats = {0};
        bool has_curr = get_all_interfaces_stats(&curr_stats);

        if (has_prev && has_curr) {
            unsigned long long delta_rx = 0;
            unsigned long long delta_tx = 0;

            if (curr_stats.rx_bytes >= prev_stats.rx_bytes) {
                delta_rx = curr_stats.rx_bytes - prev_stats.rx_bytes;
            }
            if (curr_stats.tx_bytes >= prev_stats.tx_bytes) {
                delta_tx = curr_stats.tx_bytes - prev_stats.tx_bytes;
            }

            if (delta_rx > 0 || delta_tx > 0) {
                SocketList sock_list;
                init_socket_list(&sock_list);

                parse_proc_net_file("/proc/net/tcp", &sock_list);
                parse_proc_net_file("/proc/net/tcp6", &sock_list);
                parse_proc_net_file("/proc/net/udp", &sock_list);
                parse_proc_net_file("/proc/net/udp6", &sock_list);

                sample_process_activity(&tracker, &sock_list);
                attribute_bandwidth(&tracker, delta_rx, delta_tx);

                free_socket_list(&sock_list);
            }
        }

        if (has_curr) {
            prev_stats = curr_stats;
            has_prev = true;
        }

        time_t now = time(NULL);
        if (now - last_flush_time >= BATCH_INTERVAL_SECONDS) {
            if (flush_tracker_to_db(db, &tracker)) {
                log_message(LOG_INFO, "Committed periodic network consumption batch to SQLite.");
            }
            last_flush_time = now;
        }
    }

    flush_tracker_to_db(db, &tracker);
    log_message(LOG_INFO, "NetMonitor background collector stopped gracefully.");
}

int get_optional_numeric_arg(int argc, char *argv[]) {
    if (optarg) {
        int n = atoi(optarg);
        return n > 0 ? n : 1;
    }
    if (optind < argc && argv[optind] && argv[optind][0] != '-') {
        int n = atoi(argv[optind]);
        if (n > 0) {
            optind++;
            return n;
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    CliOptions opts = {
        .time_filter_active = false,
        .time_clause = "",
        .time_description = "",
        .min_bytes = 0,
        .sort_asc = false,
        .run_daemon = false,
        .record_snapshot = false
    };

    enum {
        OPT_LAST_MINUTE = 1000,
        OPT_LAST_HOUR,
        OPT_MIN_SIZE,
        OPT_CLEAR
    };

    static struct option long_options[] = {
        {"last-minute", optional_argument, 0, OPT_LAST_MINUTE},
        {"last-min",    optional_argument, 0, OPT_LAST_MINUTE},
        {"last-hour",   optional_argument, 0, OPT_LAST_HOUR},
        {"last-day",    optional_argument, 0, 'd'},
        {"last-week",   optional_argument, 0, 'w'},
        {"last-month",  optional_argument, 0, 'm'},
        {"custom",      required_argument, 0, 'c'},
        {"min",         required_argument, 0, OPT_MIN_SIZE},
        {"sort",        required_argument, 0, 's'},
        {"clear",       no_argument,       0, OPT_CLEAR},
        {"reset",       no_argument,       0, OPT_CLEAR},
        {"daemon",      no_argument,       0, 'D'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "d::w::m::c:s:Dh", long_options, &option_index)) != -1) {
        switch (opt) {
            case OPT_LAST_MINUTE: {
                int n = get_optional_numeric_arg(argc, argv);
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-%d minutes', 'localtime')", n);
                snprintf(opts.time_description, sizeof(opts.time_description), "Last %d Minute(s)", n);
                break;
            }

            case OPT_LAST_HOUR: {
                int n = get_optional_numeric_arg(argc, argv);
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-%d hours', 'localtime')", n);
                snprintf(opts.time_description, sizeof(opts.time_description), "Last %d Hour(s)", n);
                break;
            }

            case 'd': {
                int n = get_optional_numeric_arg(argc, argv);
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-%d days', 'localtime')", n);
                snprintf(opts.time_description, sizeof(opts.time_description), "Last %d Day(s)", n);
                break;
            }

            case 'w': {
                int n = get_optional_numeric_arg(argc, argv);
                int days = n * 7;
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-%d days', 'localtime')", days);
                snprintf(opts.time_description, sizeof(opts.time_description), "Last %d Week(s) (%d Days)", n, days);
                break;
            }

            case 'm': {
                int n = get_optional_numeric_arg(argc, argv);
                opts.time_filter_active = true;
                snprintf(opts.time_clause, sizeof(opts.time_clause), "timestamp >= datetime('now', '-%d months', 'localtime')", n);
                snprintf(opts.time_description, sizeof(opts.time_description), "Last %d Month(s)", n);
                break;
            }

            case 'c':
                if (optarg) {
                    opts.time_filter_active = true;
                    snprintf(opts.time_clause, sizeof(opts.time_clause), "date(timestamp) = date('%s')", optarg);
                    snprintf(opts.time_description, sizeof(opts.time_description), "Custom Date: %s", optarg);
                }
                break;

            case OPT_MIN_SIZE:
                if (optarg) {
                    opts.min_bytes = parse_size_string(optarg);
                }
                break;

            case 's':
                if (optarg) {
                    if (strcasecmp(optarg, "asc") == 0) {
                        opts.sort_asc = true;
                    } else if (strcasecmp(optarg, "desc") == 0) {
                        opts.sort_asc = false;
                    } else {
                        fprintf(stderr, "Invalid sort option '%s'. Use 'asc' or 'desc'.\n", optarg);
                        return 1;
                    }
                }
                break;

            case OPT_CLEAR:
                opts.clear_history = true;
                break;

            case 'D':
                opts.run_daemon = true;
                break;

            case 'h':
                print_help(argv[0]);
                return 0;

            default:
                print_help(argv[0]);
                return 1;
        }
    }

    if (opts.clear_history) {
        Database db = {0};
        if (!init_database(&db, true)) {
            return 1;
        }
        clear_database(&db);
        close_database(&db);
        return 0;
    }

    if (opts.run_daemon) {
        is_daemon_mode = true;
        openlog("netmon", LOG_PID | LOG_CONS, LOG_DAEMON);
        daemonize();
    }

    Database db = {0};
    if (!init_database(&db, opts.run_daemon)) {
        if (is_daemon_mode) closelog();
        return 1;
    }

    if (opts.run_daemon) {
        run_daemon_collector(&db);
    } else {
        query_database(&db, &opts);
    }

    close_database(&db);
    if (is_daemon_mode) {
        closelog();
    }

    return 0;
}
