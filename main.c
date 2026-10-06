#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <getopt.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <pcap.h>
#include "sqlite3.h"

// ============================================================================
// Constants and Definitions
// ============================================================================
#define SYSTEM_DB_DIR          "/var/lib/netmon"
#define SYSTEM_DB_FILE         "/var/lib/netmon/usage.db"
#define PROC_NET_DEV           "/proc/net/dev"
#define BUFFER_SIZE            512
#define BATCH_INTERVAL_SECONDS 60

#define MAX_TRACKED_APPS       512
#define MAX_PIDS               4096
#define MAX_HOST_IPS           64
#define MAX_INTERFACES         32

#define PACKET_QUEUE_CAPACITY  65536
#define BATCH_DEQUEUE_SIZE     4096

#define INODE_HASH_BUCKETS     4096
#define MAX_INODE_ENTRIES      16384

#define SOCKET_HASH_BUCKETS    8192
#define MAX_SOCKET_ENTRIES     16384

#define DIR_UNKNOWN            0
#define DIR_TX                 1 // Outgoing (Sent / Upload)
#define DIR_RX                 2 // Incoming (Received / Download)

// ============================================================================
// Global Flags and Settings
// ============================================================================
static volatile bool keep_running = true;
static bool is_daemon_mode = false;
static bool config_treat_cgnat_as_local = false; // Default: CGNAT (100.64.0.0/10) is WAN

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
        } else if (priority == LOG_WARNING) {
            fprintf(stderr, "[WARNING] ");
            vfprintf(stderr, format, args);
            fprintf(stderr, "\n");
        } else {
            vprintf(format, args);
            printf("\n");
        }
    }
    va_end(args);
}

// ============================================================================
// System Paths & Daemonization
// ============================================================================
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
            int ret = system(cmd);
            (void)ret;
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

// ============================================================================
// SQLite Database Layer
// ============================================================================
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
            "CREATE INDEX IF NOT EXISTS idx_local_time ON network_usage(is_local, timestamp);"
            "CREATE INDEX IF NOT EXISTS idx_timestamp ON network_usage(timestamp);";

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

        chmod(database->db_path, 0666);
        char wal_path[550], shm_path[550];
        snprintf(wal_path, sizeof(wal_path), "%s-wal", database->db_path);
        snprintf(shm_path, sizeof(shm_path), "%s-shm", database->db_path);
        chmod(wal_path, 0666);
        chmod(shm_path, 0666);
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

    if (access(database->db_path, W_OK) != 0) {
        fprintf(stderr, "\n[ERROR] Cannot clear database: Write permission denied on '%s'.\n", database->db_path);
        fprintf(stderr, "[HINT] The database is owned by root. Please run with sudo:\n");
        fprintf(stderr, "       sudo netmon --clear\n\n");
        return false;
    }

    char *err_msg = NULL;
    int rc = sqlite3_exec(database->db, "DELETE FROM network_usage; VACUUM;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "\n[ERROR] Failed to clear database: %s\n", err_msg ? err_msg : "Unknown error");
        if (rc == SQLITE_READONLY || (err_msg && strstr(err_msg, "readonly"))) {
            fprintf(stderr, "[HINT] The database is opened read-only or locked by system service. Run with sudo:\n");
            fprintf(stderr, "       sudo netmon --clear\n\n");
        }
        sqlite3_free(err_msg);
        return false;
    }
    printf("===================================================================================================\n");
    printf(" [✓] Successfully cleared all network usage history from database.\n");
    printf(" Database: %s\n", database->db_path);
    printf("===================================================================================================\n");
    return true;
}

// ============================================================================
// Interface Filtering & /proc/net/dev Stats
// ============================================================================
typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} TotalNetStats;

bool is_ignored_interface(const char *name) {
    if (!name) return true;
    if (strcmp(name, "lo") == 0) return true;
    if (strcmp(name, "any") == 0) return true;
    if (strncmp(name, "veth", 4) == 0) return true;
    if (strncmp(name, "br-", 3) == 0) return true;
    if (strcmp(name, "docker0") == 0) return true;
    if (strncmp(name, "usbmon", 6) == 0) return true;
    if (strncmp(name, "bluetooth", 9) == 0) return true;
    if (strncmp(name, "nflog", 5) == 0) return true;
    if (strncmp(name, "nfqueue", 7) == 0) return true;
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
        // Corrected format specifier from %s to %31s to prevent buffer overflow
        sscanf(line, "%31s", if_name);

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

// ============================================================================
// Host Local IPs List (for Direction Resolution)
// ============================================================================
typedef struct {
    uint32_t v4[MAX_HOST_IPS];
    size_t v4_count;
    struct in6_addr v6[MAX_HOST_IPS];
    size_t v6_count;
} HostIpList;

static HostIpList g_host_ips;

void update_host_ips(HostIpList *list) {
    list->v4_count = 0;
    list->v6_count = 0;

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        return;
    }

    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name) continue;
        if (is_ignored_interface(ifa->ifa_name)) continue;

        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET && list->v4_count < MAX_HOST_IPS) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            list->v4[list->v4_count++] = sa->sin_addr.s_addr; // Network byte order
        } else if (family == AF_INET6 && list->v6_count < MAX_HOST_IPS) {
            struct sockaddr_in6 *sa = (struct sockaddr_in6 *)ifa->ifa_addr;
            memcpy(&list->v6[list->v6_count++], &sa->sin6_addr, sizeof(struct in6_addr));
        }
    }

    freeifaddrs(ifaddr);
}

bool is_host_ipv4(const HostIpList *list, uint32_t ip_net) {
    for (size_t i = 0; i < list->v4_count; i++) {
        if (list->v4[i] == ip_net) return true;
    }
    return false;
}

bool is_host_ipv6(const HostIpList *list, const struct in6_addr *ip6) {
    for (size_t i = 0; i < list->v6_count; i++) {
        if (memcmp(&list->v6[i], ip6, sizeof(struct in6_addr)) == 0) return true;
    }
    return false;
}

// ============================================================================
// LAN vs WAN IP Classification (RFC 1918, Link-Local, CGNAT, etc.)
// ============================================================================
bool is_ipv4_local(uint32_t ip_net_order) {
    const unsigned char *b = (const unsigned char *)&ip_net_order;
    unsigned char b1 = b[0];
    unsigned char b2 = b[1];

    if (b1 == 127) return true; // Loopback
    if (b1 == 10) return true;  // 10.0.0.0/8
    if (b1 == 192 && b2 == 168) return true; // 192.168.0.0/16
    if (b1 == 172 && (b2 >= 16 && b2 <= 31)) return true; // 172.16.0.0/12
    if (b1 == 169 && b2 == 254) return true; // 169.254.0.0/16 Link-Local
    if (b1 >= 224) return true; // Multicast
    if (b1 == 0 && b2 == 0) return true; // 0.0.0.0

    // CGNAT 100.64.0.0/10 (100.64.0.0 - 100.127.255.255)
    // By default treated as WAN (Internet), configurable via --cgnat-local
    if (b1 == 100 && (b2 >= 64 && b2 <= 127)) {
        return config_treat_cgnat_as_local;
    }

    return false;
}

bool is_ipv6_local(const unsigned char *ip6) {
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

    // 3. IPv4-mapped IPv6 (::ffff:w.x.y.z)
    bool is_v4_mapped = true;
    for (int i = 0; i < 10; i++) {
        if (ip6[i] != 0) { is_v4_mapped = false; break; }
    }
    if (is_v4_mapped && ip6[10] == 0xFF && ip6[11] == 0xFF) {
        uint32_t v4_net;
        memcpy(&v4_net, ip6 + 12, 4);
        return is_ipv4_local(v4_net);
    }

    // 4. fe80::/10 (Link-Local)
    if (ip6[0] == 0xFE && (ip6[1] & 0xC0) == 0x80) return true;

    // 5. fc00::/7 (ULA private IPv6)
    if ((ip6[0] & 0xFE) == 0xFC) return true;

    // 6. ff00::/8 (Multicast)
    if (ip6[0] == 0xFF) return true;

    return false;
}

bool is_hex_ip_local(const char *hex_ip) {
    if (!hex_ip) return false;
    size_t len = strlen(hex_ip);
    if (len < 8) return false;

    if (len == 32) {
        unsigned char ip6[16];
        for (int w = 0; w < 4; w++) {
            unsigned int word = 0;
            char word_str[9] = {0};
            strncpy(word_str, hex_ip + (w * 8), 8);
            if (sscanf(word_str, "%x", &word) != 1) return false;
            ip6[w * 4 + 0] = (word >> 0) & 0xFF;
            ip6[w * 4 + 1] = (word >> 8) & 0xFF;
            ip6[w * 4 + 2] = (word >> 16) & 0xFF;
            ip6[w * 4 + 3] = (word >> 24) & 0xFF;
        }
        return is_ipv6_local(ip6);
    }

    unsigned int raw_ip = 0;
    if (sscanf(hex_ip, "%x", &raw_ip) != 1) return false;
    // On x86 little endian, raw_ip parsed as %x has the exact network order layout
    return is_ipv4_local(raw_ip);
}

// ============================================================================
// Process Information, Inode Mapping & PID Reuse Protection
// ============================================================================
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

// Extracts starttime (field 22) from /proc/[pid]/stat to detect PID reuse
bool get_process_starttime(pid_t pid, unsigned long long *starttime) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    // /proc/[pid]/stat format: pid (comm) state ...
    // Since comm can contain spaces and parentheses, locate the LAST closing ')'
    char *paren = strrchr(buf, ')');
    if (!paren) return false;

    char *p = paren + 2; // Skip ") "
    // Starting at field 3 (state, index 0). Field 22 (starttime) is index 19 from here.
    for (int i = 0; i < 19; i++) {
        p = strchr(p, ' ');
        if (!p) return false;
        p++;
    }

    *starttime = strtoull(p, NULL, 10);
    return true;
}

// Inode Map: maps socket inode to (pid, comm)
typedef struct InodeEntry {
    unsigned long inode;
    pid_t pid;
    char comm[64];
    struct InodeEntry *next;
} InodeEntry;

typedef struct {
    InodeEntry *buckets[INODE_HASH_BUCKETS];
    InodeEntry pool[MAX_INODE_ENTRIES];
    size_t pool_count;
} InodeMap;

void init_inode_map(InodeMap *map) {
    map->pool_count = 0;
    memset(map->buckets, 0, sizeof(map->buckets));
}

void add_inode_mapping(InodeMap *map, unsigned long inode, pid_t pid, const char *comm) {
    if (map->pool_count >= MAX_INODE_ENTRIES || inode == 0) return;

    size_t b = inode % INODE_HASH_BUCKETS;
    InodeEntry *e = &map->pool[map->pool_count++];
    e->inode = inode;
    e->pid = pid;
    snprintf(e->comm, sizeof(e->comm), "%s", comm ? comm : "[unknown]");
    e->next = map->buckets[b];
    map->buckets[b] = e;
}

InodeEntry* lookup_inode(const InodeMap *map, unsigned long inode) {
    if (inode == 0) return NULL;
    size_t b = inode % INODE_HASH_BUCKETS;
    for (InodeEntry *e = map->buckets[b]; e != NULL; e = e->next) {
        if (e->inode == inode) return e;
    }
    return NULL;
}

// Persistent process tracking to guard against PID reuse and track process lifecycle
typedef struct {
    pid_t pid;
    unsigned long long start_time;
    char comm[64];
    int unseen_cycles;
    bool seen;
} TrackedPid;

// ============================================================================
// Socket Hash Table & 5-Tuple Matching
// ============================================================================
typedef struct SocketHashEntry {
    uint8_t ip_ver;         // 4 or 6
    uint8_t protocol;       // IPPROTO_TCP or IPPROTO_UDP
    uint16_t local_port;    // Host byte order
    uint16_t remote_port;   // Host byte order
    union {
        uint32_t v4;
        uint8_t v6[16];
    } local_ip;
    union {
        uint32_t v4;
        uint8_t v6[16];
    } remote_ip;

    unsigned long inode;
    pid_t pid;
    char comm[64];
    bool is_local;

    struct SocketHashEntry *next_exact;
    struct SocketHashEntry *next_wildcard;
} SocketHashEntry;

typedef struct {
    SocketHashEntry *exact_buckets[SOCKET_HASH_BUCKETS];
    SocketHashEntry *wildcard_buckets[SOCKET_HASH_BUCKETS];
    SocketHashEntry pool[MAX_SOCKET_ENTRIES];
    size_t count;
} SocketTable;

static inline uint32_t hash_5tuple(uint8_t proto, uint16_t lport, uint16_t rport,
                                   int ip_ver, const void *lip, const void *rip) {
    uint32_t h = 2166136261u;
    h = (h ^ proto) * 16777619u;
    h = (h ^ (lport & 0xFF)) * 16777619u;
    h = (h ^ (lport >> 8)) * 16777619u;
    h = (h ^ (rport & 0xFF)) * 16777619u;
    h = (h ^ (rport >> 8)) * 16777619u;

    size_t ip_len = (ip_ver == 4) ? 4 : 16;
    const uint8_t *p1 = (const uint8_t *)lip;
    const uint8_t *p2 = (const uint8_t *)rip;
    for (size_t i = 0; i < ip_len; i++) {
        h = (h ^ p1[i]) * 16777619u;
        h = (h ^ p2[i]) * 16777619u;
    }
    return h;
}

static inline uint32_t hash_port(uint8_t proto, uint16_t lport) {
    uint32_t h = 2166136261u;
    h = (h ^ proto) * 16777619u;
    h = (h ^ (lport & 0xFF)) * 16777619u;
    h = (h ^ (lport >> 8)) * 16777619u;
    return h;
}

void init_socket_table(SocketTable *table) {
    table->count = 0;
    memset(table->exact_buckets, 0, sizeof(table->exact_buckets));
    memset(table->wildcard_buckets, 0, sizeof(table->wildcard_buckets));
}

void add_socket_entry(SocketTable *table, uint8_t ip_ver, uint8_t proto,
                      const void *lip, uint16_t lport,
                      const void *rip, uint16_t rport,
                      unsigned long inode, pid_t pid, const char *comm, bool is_local) {
    if (table->count >= MAX_SOCKET_ENTRIES) return;

    SocketHashEntry *entry = &table->pool[table->count++];
    entry->ip_ver = ip_ver;
    entry->protocol = proto;
    entry->local_port = lport;
    entry->remote_port = rport;
    entry->inode = inode;
    entry->pid = pid;
    snprintf(entry->comm, sizeof(entry->comm), "%s", comm ? comm : "[unknown]");
    entry->is_local = is_local;
    entry->next_exact = NULL;
    entry->next_wildcard = NULL;

    size_t ip_size = (ip_ver == 4) ? 4 : 16;
    memcpy(&entry->local_ip, lip, ip_size);
    memcpy(&entry->remote_ip, rip, ip_size);

    bool is_wildcard_remote = (rport == 0);
    if (!is_wildcard_remote) {
        if (ip_ver == 4 && entry->remote_ip.v4 == 0) is_wildcard_remote = true;
        else if (ip_ver == 6) {
            bool all_zero = true;
            for (int i = 0; i < 16; i++) {
                if (entry->remote_ip.v6[i] != 0) { all_zero = false; break; }
            }
            if (all_zero) is_wildcard_remote = true;
        }
    }

    if (!is_wildcard_remote) {
        // Connected socket: add to exact 5-tuple table
        uint32_t h = hash_5tuple(proto, lport, rport, ip_ver, lip, rip) % SOCKET_HASH_BUCKETS;
        entry->next_exact = table->exact_buckets[h];
        table->exact_buckets[h] = entry;
    } else {
        // Listening or unconnected UDP socket: add to wildcard port table
        uint32_t h = hash_port(proto, lport) % SOCKET_HASH_BUCKETS;
        entry->next_wildcard = table->wildcard_buckets[h];
        table->wildcard_buckets[h] = entry;
    }
}

// ============================================================================
// Packet Event Structure & Ring Buffer Queue
// ============================================================================
typedef struct {
    uint8_t ip_ver;    // 4 or 6
    uint8_t proto;     // IPPROTO_TCP or IPPROTO_UDP
    uint8_t direction; // DIR_TX, DIR_RX, or DIR_UNKNOWN
    uint32_t length;   // Wire packet length in bytes
    uint16_t src_port; // Host byte order
    uint16_t dst_port; // Host byte order
    union {
        uint32_t v4;
        uint8_t v6[16];
    } src_ip;
    union {
        uint32_t v4;
        uint8_t v6[16];
    } dst_ip;
} PacketEvent;

typedef struct {
    PacketEvent events[PACKET_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    unsigned long long dropped_packets;
    pthread_mutex_t lock;
} PacketQueue;

static PacketQueue g_packet_queue;

void init_packet_queue(PacketQueue *queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->dropped_packets = 0;
    pthread_mutex_init(&queue->lock, NULL);
}

void destroy_packet_queue(PacketQueue *queue) {
    pthread_mutex_destroy(&queue->lock);
}

static inline void enqueue_packet(PacketQueue *queue, const PacketEvent *ev) {
    pthread_mutex_lock(&queue->lock);
    if (queue->count < PACKET_QUEUE_CAPACITY) {
        queue->events[queue->tail] = *ev;
        queue->tail = (queue->tail + 1) % PACKET_QUEUE_CAPACITY;
        queue->count++;
    } else {
        queue->dropped_packets++;
    }
    pthread_mutex_unlock(&queue->lock);
}

size_t dequeue_packet_batch(PacketQueue *queue, PacketEvent *dest, size_t max_items) {
    pthread_mutex_lock(&queue->lock);
    size_t n = queue->count < max_items ? queue->count : max_items;
    for (size_t i = 0; i < n; i++) {
        dest[i] = queue->events[queue->head];
        queue->head = (queue->head + 1) % PACKET_QUEUE_CAPACITY;
    }
    queue->count -= n;
    pthread_mutex_unlock(&queue->lock);
    return n;
}

// ============================================================================
// Libpcap Capture Workers
// ============================================================================
typedef struct {
    pcap_t *handle;
    char if_name[32];
    int datalink;
    pthread_t thread;
    bool active;
} CaptureSession;

static CaptureSession g_captures[MAX_INTERFACES];
static size_t g_capture_count = 0;

static void pcap_packet_callback(u_char *user, const struct pcap_pkthdr *h, const u_char *bytes) {
    CaptureSession *sess = (CaptureSession *)user;
    if (!bytes || h->caplen < 14) return;

    size_t offset = 0;
    uint16_t ethertype = 0;

    switch (sess->datalink) {
        case DLT_EN10MB: // Standard Ethernet
            if (h->caplen < 14) return;
            ethertype = ntohs(*(uint16_t *)(bytes + 12));
            offset = 14;
            if (ethertype == 0x8100) { // 802.1Q VLAN
                if (h->caplen < 18) return;
                ethertype = ntohs(*(uint16_t *)(bytes + 16));
                offset = 18;
            }
            break;

        case DLT_LINUX_SLL: // Linux cooked sockets v1
            if (h->caplen < 16) return;
            ethertype = ntohs(*(uint16_t *)(bytes + 14));
            offset = 16;
            break;

#ifdef DLT_LINUX_SLL2
        case DLT_LINUX_SLL2: // Linux cooked sockets v2
            if (h->caplen < 20) return;
            ethertype = ntohs(*(uint16_t *)(bytes + 0));
            offset = 20;
            break;
#endif

        case DLT_RAW:
            offset = 0;
            ethertype = (bytes[0] >> 4 == 4) ? 0x0800 : 0x86DD;
            break;

        case DLT_NULL:
            if (h->caplen < 4) return;
            offset = 4;
            ethertype = 0x0800;
            break;

        default:
            return;
    }

    PacketEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.length = h->len; // Actual wire packet length

    if (ethertype == 0x0800) { // IPv4
        if (h->caplen < offset + 20) return;
        const u_char *ip = bytes + offset;
        uint8_t ihl = (ip[0] & 0x0F) * 4;
        if (ihl < 20 || h->caplen < offset + ihl + 4) return;

        ev.ip_ver = 4;
        ev.proto = ip[9];
        if (ev.proto != IPPROTO_TCP && ev.proto != IPPROTO_UDP) return;

        memcpy(&ev.src_ip.v4, ip + 12, 4);
        memcpy(&ev.dst_ip.v4, ip + 16, 4);

        const u_char *trans = ip + ihl;
        ev.src_port = ntohs(*(uint16_t *)(trans + 0));
        ev.dst_port = ntohs(*(uint16_t *)(trans + 2));

        // Determine direction: TX if src is host IP, RX if dst is host IP
        if (is_host_ipv4(&g_host_ips, ev.src_ip.v4)) {
            ev.direction = DIR_TX;
        } else if (is_host_ipv4(&g_host_ips, ev.dst_ip.v4)) {
            ev.direction = DIR_RX;
        } else {
            ev.direction = DIR_UNKNOWN;
        }

        enqueue_packet(&g_packet_queue, &ev);

    } else if (ethertype == 0x86DD) { // IPv6
        if (h->caplen < offset + 40 + 4) return;
        const u_char *ip = bytes + offset;

        ev.ip_ver = 6;
        ev.proto = ip[6];
        if (ev.proto != IPPROTO_TCP && ev.proto != IPPROTO_UDP) return;

        memcpy(ev.src_ip.v6, ip + 8, 16);
        memcpy(ev.dst_ip.v6, ip + 24, 16);

        const u_char *trans = ip + 40;
        ev.src_port = ntohs(*(uint16_t *)(trans + 0));
        ev.dst_port = ntohs(*(uint16_t *)(trans + 2));

        if (is_host_ipv6(&g_host_ips, (const struct in6_addr *)ev.src_ip.v6)) {
            ev.direction = DIR_TX;
        } else if (is_host_ipv6(&g_host_ips, (const struct in6_addr *)ev.dst_ip.v6)) {
            ev.direction = DIR_RX;
        } else {
            ev.direction = DIR_UNKNOWN;
        }

        enqueue_packet(&g_packet_queue, &ev);
    }
}

static void* pcap_worker_thread(void *arg) {
    CaptureSession *sess = (CaptureSession *)arg;
    pcap_loop(sess->handle, -1, pcap_packet_callback, (u_char *)sess);
    return NULL;
}

bool start_packet_captures(void) {
    char errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_if_t *alldevs = NULL;

    if (pcap_findalldevs(&alldevs, errbuf) != 0) {
        log_message(LOG_ERR, "pcap_findalldevs failed: %s", errbuf);
        return false;
    }

    g_capture_count = 0;
    size_t permission_errors = 0;

    for (pcap_if_t *dev = alldevs; dev != NULL && g_capture_count < MAX_INTERFACES; dev = dev->next) {
        if (!dev->name) continue;
        if (is_ignored_interface(dev->name)) continue;
        if (dev->flags & PCAP_IF_LOOPBACK) continue;

        pcap_t *handle = pcap_create(dev->name, errbuf);
        if (!handle) {
            log_message(LOG_WARNING, "pcap_create failed on interface '%s': %s", dev->name, errbuf);
            continue;
        }

        // 96-byte snaplen: captures link layer, IP, and TCP/UDP headers without copying payload
        pcap_set_snaplen(handle, 96);
        pcap_set_promisc(handle, 0);
        pcap_set_timeout(handle, 100);
        pcap_set_immediate_mode(handle, 1);
        pcap_set_buffer_size(handle, 2 * 1024 * 1024); // 2MB ring buffer to handle bursts

        int status = pcap_activate(handle);
        if (status != 0) {
            if (status == PCAP_ERROR_PERM_DENIED || errno == EPERM) {
                permission_errors++;
                log_message(LOG_WARNING, "Permission denied capturing on interface '%s'. Requires CAP_NET_RAW or root.", dev->name);
            } else {
                log_message(LOG_WARNING, "Failed to activate pcap on '%s': %s", dev->name, pcap_geterr(handle));
            }
            pcap_close(handle);
            continue;
        }

        // BPF Filter: capture TCP and UDP only
        struct bpf_program bpf;
        if (pcap_compile(handle, &bpf, "tcp or udp", 1, PCAP_NETMASK_UNKNOWN) == 0) {
            pcap_setfilter(handle, &bpf);
            pcap_freecode(&bpf);
        }

        CaptureSession *sess = &g_captures[g_capture_count];
        sess->handle = handle;
        snprintf(sess->if_name, sizeof(sess->if_name), "%s", dev->name);
        sess->datalink = pcap_datalink(handle);
        sess->active = true;

        if (pthread_create(&sess->thread, NULL, pcap_worker_thread, sess) == 0) {
            g_capture_count++;
            log_message(LOG_INFO, "Active packet capture started on interface '%s' (datalink: %d).", dev->name, sess->datalink);
        } else {
            log_message(LOG_ERR, "Failed to create capture worker thread for '%s'.", dev->name);
            pcap_close(handle);
        }
    }

    pcap_freealldevs(alldevs);

    if (g_capture_count == 0) {
        if (permission_errors > 0 || geteuid() != 0) {
            log_message(LOG_ERR, "CRITICAL: Insufficient permissions to capture network packets. Run with CAP_NET_RAW capability or as root.");
        } else {
            log_message(LOG_ERR, "CRITICAL: No active network interfaces available for packet capture.");
        }
        return false;
    }

    return true;
}

void stop_packet_captures(void) {
    for (size_t i = 0; i < g_capture_count; i++) {
        if (g_captures[i].active && g_captures[i].handle) {
            pcap_breakloop(g_captures[i].handle);
        }
    }

    for (size_t i = 0; i < g_capture_count; i++) {
        if (g_captures[i].active) {
            pthread_join(g_captures[i].thread, NULL);
            if (g_captures[i].handle) {
                pcap_close(g_captures[i].handle);
                g_captures[i].handle = NULL;
            }
            g_captures[i].active = false;
        }
    }
    g_capture_count = 0;
}

// ============================================================================
// ProcFS Scanning & Socket Table Building
// ============================================================================
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
    TrackedPid pids[MAX_PIDS];
    size_t pid_count;
} BandwidthTracker;

void init_tracker(BandwidthTracker *tracker) {
    tracker->count = 0;
    tracker->pid_count = 0;
    memset(tracker->apps, 0, sizeof(tracker->apps));
    memset(tracker->pids, 0, sizeof(tracker->pids));
}

AppBandwidth* get_or_create_app(BandwidthTracker *tracker, const char *name) {
    const char *target_name = (name && name[0]) ? name : "[unknown]";
    for (size_t i = 0; i < tracker->count; i++) {
        if (strcmp(tracker->apps[i].app_name, target_name) == 0) {
            return &tracker->apps[i];
        }
    }
    if (tracker->count < MAX_TRACKED_APPS) {
        size_t idx = tracker->count++;
        snprintf(tracker->apps[idx].app_name, sizeof(tracker->apps[idx].app_name), "%s", target_name);
        tracker->apps[idx].wan_upload = 0;
        tracker->apps[idx].wan_download = 0;
        tracker->apps[idx].lan_upload = 0;
        tracker->apps[idx].lan_download = 0;
        return &tracker->apps[idx];
    }
    return NULL;
}

// Scans /proc to link socket inodes with processes, validating start_time to prevent PID reuse
void scan_proc_inodes(InodeMap *map, BandwidthTracker *tracker) {
    init_inode_map(map);

    for (size_t i = 0; i < tracker->pid_count; i++) {
        tracker->pids[i].seen = false;
    }

    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return;

    struct dirent *proc_entry;
    while ((proc_entry = readdir(proc_dir)) != NULL) {
        if (!isdigit(proc_entry->d_name[0])) continue;

        pid_t pid = (pid_t)atoi(proc_entry->d_name);
        if (pid <= 0) continue;

        unsigned long long current_start = 0;
        if (!get_process_starttime(pid, &current_start)) {
            continue;
        }

        // Find or create tracked PID entry
        TrackedPid *tp = NULL;
        for (size_t i = 0; i < tracker->pid_count; i++) {
            if (tracker->pids[i].pid == pid) {
                tp = &tracker->pids[i];
                break;
            }
        }

        if (tp) {
            // PID reuse detection: if start_time changed, refresh comm
            if (tp->start_time != current_start) {
                tp->start_time = current_start;
                get_process_comm(pid, tp->comm, sizeof(tp->comm));
            }
            tp->seen = true;
            tp->unseen_cycles = 0;
        } else if (tracker->pid_count < MAX_PIDS) {
            tp = &tracker->pids[tracker->pid_count++];
            tp->pid = pid;
            tp->start_time = current_start;
            get_process_comm(pid, tp->comm, sizeof(tp->comm));
            tp->seen = true;
            tp->unseen_cycles = 0;
        }

        const char *comm = tp ? tp->comm : "[unknown]";

        // Scan file descriptors
        char fd_dir_path[256];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);

        DIR *fd_dir = opendir(fd_dir_path);
        if (!fd_dir) continue;

        struct dirent *fd_entry;
        while ((fd_entry = readdir(fd_dir)) != NULL) {
            if (fd_entry->d_name[0] == '.') continue;

            char link_path[512];
            snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir_path, fd_entry->d_name);

            char link_target[256];
            ssize_t len = readlink(link_path, link_target, sizeof(link_target) - 1);
            if (len <= 0) continue;
            link_target[len] = '\0';

            unsigned long inode = 0;
            if (sscanf(link_target, "socket:[%lu]", &inode) == 1 && inode > 0) {
                add_inode_mapping(map, inode, pid, comm);
            }
        }
        closedir(fd_dir);
    }
    closedir(proc_dir);

    // Memory cleanup: prune dead processes not seen for 5 consecutive cycles (5 seconds)
    size_t i = 0;
    while (i < tracker->pid_count) {
        if (!tracker->pids[i].seen) {
            tracker->pids[i].unseen_cycles++;
            if (tracker->pids[i].unseen_cycles >= 5) {
                tracker->pids[i] = tracker->pids[tracker->pid_count - 1];
                tracker->pid_count--;
                continue;
            }
        }
        i++;
    }
}

// Parses /proc/net/tcp, tcp6, udp, udp6 and populates the SocketTable
void parse_proc_net_file(const char *path, uint8_t ip_ver, uint8_t proto,
                         const InodeMap *inode_map, SocketTable *table) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[BUFFER_SIZE];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        unsigned long inode = 0;
        char local_addr_str[128], rem_addr_str[128];
        unsigned int state = 0;
        unsigned long tx_q = 0, rx_q = 0;
        int dummy_d1 = 0, dummy_d2 = 0;
        unsigned long dummy_ul1 = 0, dummy_ul2 = 0, dummy_ul3 = 0;

        int num_matched = sscanf(line,
               "%*d: %127s %127s %x %lx:%lx %lx:%lx %lx %d %d %lu",
               local_addr_str, rem_addr_str, &state, &tx_q, &rx_q,
               &dummy_ul1, &dummy_ul2, &dummy_ul3,
               &dummy_d1, &dummy_d2, &inode);

        if (num_matched >= 11 && inode > 0) {
            char *colon_loc = strchr(local_addr_str, ':');
            char *colon_rem = strchr(rem_addr_str, ':');
            if (!colon_loc || !colon_rem) continue;

            *colon_loc = '\0';
            *colon_rem = '\0';

            unsigned int lport = 0, rport = 0;
            sscanf(colon_loc + 1, "%x", &lport);
            sscanf(colon_rem + 1, "%x", &rport);

            union { uint32_t v4; uint8_t v6[16]; } lip;
            union { uint32_t v4; uint8_t v6[16]; } rip;
            memset(&lip, 0, sizeof(lip));
            memset(&rip, 0, sizeof(rip));

            if (ip_ver == 4) {
                unsigned int raw_lip = 0, raw_rip = 0;
                sscanf(local_addr_str, "%x", &raw_lip);
                sscanf(rem_addr_str, "%x", &raw_rip);
                lip.v4 = raw_lip;
                rip.v4 = raw_rip;
            } else {
                for (int w = 0; w < 4; w++) {
                    unsigned int word_l = 0, word_r = 0;
                    char wstr_l[9] = {0}, wstr_r[9] = {0};
                    memcpy(wstr_l, local_addr_str + (w * 8), 8);
                    memcpy(wstr_r, rem_addr_str + (w * 8), 8);
                    wstr_l[8] = '\0';
                    wstr_r[8] = '\0';
                    sscanf(wstr_l, "%x", &word_l);
                    sscanf(wstr_r, "%x", &word_r);
                    lip.v6[w * 4 + 0] = (word_l >> 0) & 0xFF;
                    lip.v6[w * 4 + 1] = (word_l >> 8) & 0xFF;
                    lip.v6[w * 4 + 2] = (word_l >> 16) & 0xFF;
                    lip.v6[w * 4 + 3] = (word_l >> 24) & 0xFF;
                    rip.v6[w * 4 + 0] = (word_r >> 0) & 0xFF;
                    rip.v6[w * 4 + 1] = (word_r >> 8) & 0xFF;
                    rip.v6[w * 4 + 2] = (word_r >> 16) & 0xFF;
                    rip.v6[w * 4 + 3] = (word_r >> 24) & 0xFF;
                }
            }

            bool is_local = is_hex_ip_local(rem_addr_str);
            if (rport == 1716 || rport == 5353) {
                is_local = true;
            }

            InodeEntry *ie = lookup_inode(inode_map, inode);
            pid_t pid = ie ? ie->pid : 0;
            const char *comm = ie ? ie->comm : "system-network";

            add_socket_entry(table, ip_ver, proto,
                             &lip, (uint16_t)lport,
                             &rip, (uint16_t)rport,
                             inode, pid, comm, is_local);
        }
    }

    fclose(f);
}

void build_socket_table(SocketTable *table, const InodeMap *inode_map) {
    init_socket_table(table);
    parse_proc_net_file("/proc/net/tcp", 4, IPPROTO_TCP, inode_map, table);
    parse_proc_net_file("/proc/net/tcp6", 6, IPPROTO_TCP, inode_map, table);
    parse_proc_net_file("/proc/net/udp", 4, IPPROTO_UDP, inode_map, table);
    parse_proc_net_file("/proc/net/udp6", 6, IPPROTO_UDP, inode_map, table);
}

// ============================================================================
// Packet to Socket Matching & Aggregation
// ============================================================================
static SocketHashEntry* match_candidate_socket(const SocketTable *table,
                                               uint8_t ip_ver, uint8_t proto,
                                               const void *cand_lip, uint16_t cand_lport,
                                               const void *cand_rip, uint16_t cand_rport) {
    size_t ip_size = (ip_ver == 4) ? 4 : 16;

    // 1. Exact 5-tuple lookup
    uint32_t h_exact = hash_5tuple(proto, cand_lport, cand_rport, ip_ver, cand_lip, cand_rip) % SOCKET_HASH_BUCKETS;
    for (SocketHashEntry *e = table->exact_buckets[h_exact]; e != NULL; e = e->next_exact) {
        if (e->ip_ver == ip_ver && e->protocol == proto &&
            e->local_port == cand_lport && e->remote_port == cand_rport &&
            memcmp(&e->local_ip, cand_lip, ip_size) == 0 &&
            memcmp(&e->remote_ip, cand_rip, ip_size) == 0) {
            return e;
        }
    }

    // 2. Wildcard port lookup (unconnected UDP or listening socket)
    uint32_t h_wild = hash_port(proto, cand_lport) % SOCKET_HASH_BUCKETS;
    SocketHashEntry *best_match = NULL;
    for (SocketHashEntry *e = table->wildcard_buckets[h_wild]; e != NULL; e = e->next_wildcard) {
        if (e->ip_ver == ip_ver && e->protocol == proto && e->local_port == cand_lport) {
            // Check if local IP matches, or if socket bound to INADDR_ANY (all zeros)
            if (memcmp(&e->local_ip, cand_lip, ip_size) == 0) {
                return e; // Exact local IP match on listening socket
            }
            if (!best_match) {
                best_match = e; // Wildcard bound fallback
            }
        }
    }

    return best_match;
}

void process_packet_event(BandwidthTracker *tracker, const SocketTable *table, const PacketEvent *ev) {
    SocketHashEntry *matched = NULL;
    int resolved_dir = ev->direction;
    const void *rem_ip = NULL;

    if (ev->direction == DIR_TX) {
        matched = match_candidate_socket(table, ev->ip_ver, ev->proto,
                                         &ev->src_ip, ev->src_port,
                                         &ev->dst_ip, ev->dst_port);
        rem_ip = &ev->dst_ip;
    } else if (ev->direction == DIR_RX) {
        matched = match_candidate_socket(table, ev->ip_ver, ev->proto,
                                         &ev->dst_ip, ev->dst_port,
                                         &ev->src_ip, ev->src_port);
        rem_ip = &ev->src_ip;
    } else {
        // Unknown direction: try TX assumption first, then RX
        matched = match_candidate_socket(table, ev->ip_ver, ev->proto,
                                         &ev->src_ip, ev->src_port,
                                         &ev->dst_ip, ev->dst_port);
        if (matched) {
            resolved_dir = DIR_TX;
            rem_ip = &ev->dst_ip;
        } else {
            matched = match_candidate_socket(table, ev->ip_ver, ev->proto,
                                             &ev->dst_ip, ev->dst_port,
                                             &ev->src_ip, ev->src_port);
            if (matched) {
                resolved_dir = DIR_RX;
                rem_ip = &ev->src_ip;
            } else {
                resolved_dir = DIR_RX;
                rem_ip = &ev->src_ip;
            }
        }
    }

    const char *app_name = matched ? matched->comm : "system-network";
    AppBandwidth *app = get_or_create_app(tracker, app_name);
    if (!app) return;

    // Determine LAN vs WAN from the remote IP of the actual captured packet
    bool is_local = false;
    if (ev->ip_ver == 4) {
        uint32_t rip_v4 = *(const uint32_t *)rem_ip;
        is_local = is_ipv4_local(rip_v4);
    } else {
        is_local = is_ipv6_local((const unsigned char *)rem_ip);
    }

    uint16_t rem_port = (resolved_dir == DIR_TX) ? ev->dst_port : ev->src_port;
    if (rem_port == 1716 || rem_port == 5353) {
        is_local = true;
    }

    // Direct aggregation: accumulate exact wire bytes into the appropriate bucket
    if (resolved_dir == DIR_TX) {
        if (is_local) {
            app->lan_upload += ev->length;
        } else {
            app->wan_upload += ev->length;
        }
    } else {
        if (is_local) {
            app->lan_download += ev->length;
        } else {
            app->wan_download += ev->length;
        }
    }
}

// ============================================================================
// Database Persistence (Batch Commit)
// ============================================================================
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
            sqlite3_bind_int(database->insert_stmt, 4, 0); // is_local = 0 (WAN)

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
            sqlite3_bind_int(database->insert_stmt, 4, 1); // is_local = 1 (LAN)

            sqlite3_step(database->insert_stmt);

            tracker->apps[i].lan_upload = 0;
            tracker->apps[i].lan_download = 0;
        }
    }

    sqlite3_exec(database->db, "COMMIT;", NULL, NULL, NULL);
    return true;
}

// ============================================================================
// Formatting Helpers
// ============================================================================
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

// ============================================================================
// Daemon Background Collector
// ============================================================================
void run_daemon_collector(Database *db) {
    log_message(LOG_INFO, "NetMonitor actual packet capture collector started (DB: %s).", db->db_path);

    init_packet_queue(&g_packet_queue);
    update_host_ips(&g_host_ips);

    if (!start_packet_captures()) {
        log_message(LOG_ERR, "Failed to start packet captures. Exiting collector.");
        destroy_packet_queue(&g_packet_queue);
        return;
    }

    BandwidthTracker tracker;
    init_tracker(&tracker);

    static InodeMap inode_map;
    static SocketTable socket_table;

    // Perform initial socket table build
    scan_proc_inodes(&inode_map, &tracker);
    build_socket_table(&socket_table, &inode_map);

    time_t last_flush_time = time(NULL);
    time_t last_scan_time = time(NULL);

    PacketEvent batch[BATCH_DEQUEUE_SIZE];

    while (keep_running) {
        usleep(100000); // 100ms processing cadence to minimize latency and CPU usage

        // Drain available packets from the ring buffer
        size_t n = 0;
        while ((n = dequeue_packet_batch(&g_packet_queue, batch, BATCH_DEQUEUE_SIZE)) > 0) {
            for (size_t i = 0; i < n; i++) {
                process_packet_event(&tracker, &socket_table, &batch[i]);
            }
        }

        time_t now = time(NULL);

        // Periodically refresh host IPs, inode map, and socket table (every 1 second)
        if (now - last_scan_time >= 1) {
            update_host_ips(&g_host_ips);
            scan_proc_inodes(&inode_map, &tracker);
            build_socket_table(&socket_table, &inode_map);
            last_scan_time = now;
        }

        // Commit periodic batch to SQLite (every 60 seconds)
        if (now - last_flush_time >= BATCH_INTERVAL_SECONDS) {
            if (flush_tracker_to_db(db, &tracker)) {
                log_message(LOG_INFO, "Committed periodic network consumption batch to SQLite.");
            }
            last_flush_time = now;
        }
    }

    log_message(LOG_INFO, "Shutting down packet capture workers...");
    stop_packet_captures();

    // Drain any remaining packets in queue
    size_t n = 0;
    while ((n = dequeue_packet_batch(&g_packet_queue, batch, BATCH_DEQUEUE_SIZE)) > 0) {
        for (size_t i = 0; i < n; i++) {
            process_packet_event(&tracker, &socket_table, &batch[i]);
        }
    }

    flush_tracker_to_db(db, &tracker);
    destroy_packet_queue(&g_packet_queue);
    log_message(LOG_INFO, "NetMonitor background collector stopped gracefully.");
}

// ============================================================================
// SQL Query & CLI Filter Handler
// ============================================================================
typedef enum {
    GROUP_BY_APP = 0,
    GROUP_BY_DAY,
    GROUP_BY_HOUR,
    GROUP_BY_WEEK,
    GROUP_BY_MONTH
} GroupByMode;

typedef enum {
    CLEANUP_NONE = 0,
    CLEANUP_ALL,
    CLEANUP_KEEP_DAYS,
    CLEANUP_KEEP_MONTHS,
    CLEANUP_KEEP_RANGE
} CleanupMode;

typedef struct {
    bool time_filter_active;
    char time_clause[128];
    char time_description[128];
    unsigned long long min_bytes;
    bool sort_asc;
    bool sort_specified;
    GroupByMode group_by;
    bool run_daemon;
    bool record_snapshot;
    CleanupMode cleanup_mode;
    int cleanup_days;
    int cleanup_months;
    char cleanup_start[32];
    char cleanup_end[32];
    bool skip_confirm;
} CliOptions;

void print_help(const char *prog_name) {
    printf("===================================================================================================\n");
    printf("     NetMonitor - Accurate Packet-Captured Network Usage Monitor (CLI)                             \n");
    printf("===================================================================================================\n");
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Grouping & Aggregation Options:\n");
    printf("  -g, --group-by <MODE>       Group usage by: app (default), day, hour, week, month\n\n");
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
    printf("Database Retention & Cleanup (with Confirmation):\n");
    printf("      --clear, --reset        Purge ALL historical records from database (complete reset)\n");
    printf("      --keep-days <N>         Keep only the last N days (delete older records)\n");
    printf("      --keep-months <N>       Keep only the last N months (delete older records)\n");
    printf("      --keep-range <S> <E>    Keep records between START and END dates (YYYY-MM-DD)\n");
    printf("                              and delete all records outside this range\n");
    printf("  -y, --yes, --force          Skip interactive confirmation prompt\n\n");
    printf("Network Classification Options:\n");
    printf("      --cgnat-local           Treat CGNAT (100.64.0.0/10) as local LAN traffic (e.g. Tailscale)\n\n");
    printf("Daemon & Execution Modes:\n");
    printf("  -D, --daemon                Run as background daemon service (logs to syslog)\n");
    printf("  -h, --help                  Display this help message and exit\n\n");
    printf("Examples:\n");
    printf("  %s --group-by day\n", prog_name);
    printf("  %s --group-by hour --last-day 1\n", prog_name);
    printf("  %s --keep-days 10\n", prog_name);
    printf("  %s --keep-months 1\n", prog_name);
    printf("  %s --keep-range \"2026-10-01\" \"2026-10-06\"\n", prog_name);
    printf("  %s --clear\n", prog_name);
    printf("===================================================================================================\n");
}

void query_database(Database *database, const CliOptions *opts) {
    char sql[1536];
    char where_clause[512] = "";
    char having_clause[256] = "";

    if (opts->time_filter_active && strlen(opts->time_clause) > 0) {
        snprintf(where_clause, sizeof(where_clause), "WHERE %s", opts->time_clause);
    }

    if (opts->min_bytes > 0) {
        snprintf(having_clause, sizeof(having_clause), "HAVING total_bytes >= %llu", opts->min_bytes);
    }

    const char *entity_expr = "app_name";
    const char *header_title = "APPLICATION";
    const char *default_order = "total_bytes DESC";

    switch (opts->group_by) {
        case GROUP_BY_DAY:
            entity_expr = "strftime('%Y-%m-%d', timestamp)";
            header_title = "DATE / DAY";
            default_order = "entity_name DESC";
            break;
        case GROUP_BY_HOUR:
            entity_expr = "strftime('%Y-%m-%d %H:00', timestamp)";
            header_title = "HOUR PERIOD";
            default_order = "entity_name DESC";
            break;
        case GROUP_BY_WEEK:
            entity_expr = "strftime('%Y-W%W', timestamp)";
            header_title = "WEEK";
            default_order = "entity_name DESC";
            break;
        case GROUP_BY_MONTH:
            entity_expr = "strftime('%Y-%m', timestamp)";
            header_title = "MONTH";
            default_order = "entity_name DESC";
            break;
        case GROUP_BY_APP:
        default:
            entity_expr = "app_name";
            header_title = "APPLICATION";
            default_order = "total_bytes DESC, app_name ASC";
            break;
    }

    char order_by_clause[128];
    if (opts->sort_specified) {
        snprintf(order_by_clause, sizeof(order_by_clause), "total_bytes %s", opts->sort_asc ? "ASC" : "DESC");
    } else {
        snprintf(order_by_clause, sizeof(order_by_clause), "%s", default_order);
    }

    // Conditional SQL aggregation cleanly separates WAN and LAN columns in a single query
    snprintf(sql,
             sizeof(sql),
             "SELECT %s AS entity_name, "
             "       SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS wan_download, "
             "       SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS wan_upload, "
             "       SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_download, "
             "       SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_upload, "
             "       SUM(bytes_sent + bytes_received) AS total_bytes, "
             "       COUNT(*) AS entries_count, "
             "       MAX(timestamp) AS last_seen "
             "FROM network_usage "
             "%s "
             "GROUP BY entity_name "
             "%s "
             "ORDER BY %s;",
             entity_expr, where_clause, having_clause, order_by_clause);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(database->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_message(LOG_ERR, "Failed to execute database query: %s", sqlite3_errmsg(database->db));
        return;
    }

    printf("\n=====================================================================================================================================================\n");
    printf("                                              DATABASE QUERY RESULTS                                                                                 \n");
    printf("=====================================================================================================================================================\n");
    printf(" Database: %s\n", database->db_path);
    printf(" Group By: %s\n", header_title);
    printf(" Filter  : %s\n", opts->time_filter_active ? opts->time_description : "All Recorded Time");
    if (opts->min_bytes > 0) {
        char min_str[32];
        format_bytes(opts->min_bytes, min_str, sizeof(min_str));
        printf(" Min Size: >= %s\n", min_str);
    }
    printf(" Sort    : %s\n", order_by_clause);
    printf("-----------------------------------------------------------------------------------------------------------------------------------------------------\n");
    printf("%-20s %-13s %-12s %-12s %-13s %-12s %-12s %-13s %-9s %-19s\n",
           header_title, "TOTAL WAN", "WAN RX", "WAN TX", "TOTAL LAN", "LAN RX", "LAN TX", "TOTAL USAGE", "SAMPLES", "LAST SEEN");
    printf("-----------------------------------------------------------------------------------------------------------------------------------------------------\n");

    int row_count = 0;
    unsigned long long grand_wan_down = 0;
    unsigned long long grand_wan_up = 0;
    unsigned long long grand_lan_down = 0;
    unsigned long long grand_lan_up = 0;
    unsigned long long grand_total = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *entity = sqlite3_column_text(stmt, 0);
        unsigned long long wan_down = (unsigned long long)sqlite3_column_int64(stmt, 1);
        unsigned long long wan_up   = (unsigned long long)sqlite3_column_int64(stmt, 2);
        unsigned long long lan_down = (unsigned long long)sqlite3_column_int64(stmt, 3);
        unsigned long long lan_up   = (unsigned long long)sqlite3_column_int64(stmt, 4);
        unsigned long long total    = (unsigned long long)sqlite3_column_int64(stmt, 5);
        int samples = sqlite3_column_int(stmt, 6);
        const unsigned char *last_seen = sqlite3_column_text(stmt, 7);

        unsigned long long wan_tot = wan_down + wan_up;
        unsigned long long lan_tot = lan_down + lan_up;

        char wan_tot_str[32], wan_down_str[32], wan_up_str[32];
        char lan_tot_str[32], lan_down_str[32], lan_up_str[32];
        char total_str[32];

        format_bytes(wan_tot, wan_tot_str, sizeof(wan_tot_str));
        format_bytes(wan_down, wan_down_str, sizeof(wan_down_str));
        format_bytes(wan_up, wan_up_str, sizeof(wan_up_str));
        format_bytes(lan_tot, lan_tot_str, sizeof(lan_tot_str));
        format_bytes(lan_down, lan_down_str, sizeof(lan_down_str));
        format_bytes(lan_up, lan_up_str, sizeof(lan_up_str));
        format_bytes(total, total_str, sizeof(total_str));

        printf("%-20s %-13s %-12s %-12s %-13s %-12s %-12s %-13s %-9d %-19s\n",
               entity ? (const char *)entity : "[unknown]",
               wan_tot_str,
               wan_down_str,
               wan_up_str,
               lan_tot_str,
               lan_down_str,
               lan_up_str,
               total_str,
               samples,
               last_seen ? (const char *)last_seen : "-");

        grand_wan_down += wan_down;
        grand_wan_up   += wan_up;
        grand_lan_down += lan_down;
        grand_lan_up   += lan_up;
        grand_total    += total;
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0) {
        printf("  No matching records found in database for the given criteria.\n");
        printf("=====================================================================================================================================================\n\n");
        return;
    }

    printf("-----------------------------------------------------------------------------------------------------------------------------------------------------\n");
    char g_wtot_str[32], g_wdown_str[32], g_wup_str[32];
    char g_ltot_str[32], g_ldown_str[32], g_lup_str[32], g_tot_str[32];
    format_bytes(grand_wan_down + grand_wan_up, g_wtot_str, sizeof(g_wtot_str));
    format_bytes(grand_wan_down, g_wdown_str, sizeof(g_wdown_str));
    format_bytes(grand_wan_up, g_wup_str, sizeof(g_wup_str));
    format_bytes(grand_lan_down + grand_lan_up, g_ltot_str, sizeof(g_ltot_str));
    format_bytes(grand_lan_down, g_ldown_str, sizeof(g_ldown_str));
    format_bytes(grand_lan_up, g_lup_str, sizeof(g_lup_str));
    format_bytes(grand_total, g_tot_str, sizeof(g_tot_str));

    printf("%-20s %-13s %-12s %-12s %-13s %-12s %-12s %-13s Total Items: %d\n",
           "TOTAL AGGREGATED", g_wtot_str, g_wdown_str, g_wup_str, g_ltot_str, g_ldown_str, g_lup_str, g_tot_str, row_count);
    printf("-----------------------------------------------------------------------------------------------------------------------------------------------------\n");

    unsigned long long wan_tot = grand_wan_down + grand_wan_up;
    unsigned long long lan_tot = grand_lan_down + grand_lan_up;
    char wan_tot_str[32], lan_tot_str[32];
    format_bytes(wan_tot, wan_tot_str, sizeof(wan_tot_str));
    format_bytes(lan_tot, lan_tot_str, sizeof(lan_tot_str));

    printf("  🌐 Internet / WAN (Quota Usage)    : %-10s (Download: %-9s | Upload: %-9s)\n",
           wan_tot_str, g_wdown_str, g_wup_str);
    printf("  🏠 Local / LAN    (Network Sharing): %-10s (Download: %-9s | Upload: %-9s)\n",
           lan_tot_str, g_ldown_str, g_lup_str);

    printf("=====================================================================================================================================================\n\n");
}

bool purge_database_records(Database *database, const CliOptions *opts) {
    if (!database->db) return false;

    if (access(database->db_path, W_OK) != 0) {
        fprintf(stderr, "\n[ERROR] Cannot modify database: Write permission denied on '%s'.\n", database->db_path);
        fprintf(stderr, "[HINT] The database is owned by root. Please run with sudo:\n");
        fprintf(stderr, "       sudo netmon ...\n\n");
        return false;
    }

    char delete_sql[512] = "";
    char count_del_sql[512] = "";
    char desc[256] = "";

    if (opts->cleanup_mode == CLEANUP_ALL) {
        snprintf(delete_sql, sizeof(delete_sql), "DELETE FROM network_usage; VACUUM;");
        snprintf(count_del_sql, sizeof(count_del_sql), "SELECT COUNT(*) FROM network_usage;");
        snprintf(desc, sizeof(desc), "Purge ALL recorded history (complete reset)");
    } else if (opts->cleanup_mode == CLEANUP_KEEP_DAYS) {
        snprintf(delete_sql, sizeof(delete_sql),
                 "DELETE FROM network_usage WHERE timestamp < datetime('now', '-%d days', 'localtime'); VACUUM;",
                 opts->cleanup_days);
        snprintf(count_del_sql, sizeof(count_del_sql),
                 "SELECT COUNT(*) FROM network_usage WHERE timestamp < datetime('now', '-%d days', 'localtime');",
                 opts->cleanup_days);
        snprintf(desc, sizeof(desc), "Keep last %d days (purge records older than %d days)",
                 opts->cleanup_days, opts->cleanup_days);
    } else if (opts->cleanup_mode == CLEANUP_KEEP_MONTHS) {
        snprintf(delete_sql, sizeof(delete_sql),
                 "DELETE FROM network_usage WHERE timestamp < datetime('now', '-%d months', 'localtime'); VACUUM;",
                 opts->cleanup_months);
        snprintf(count_del_sql, sizeof(count_del_sql),
                 "SELECT COUNT(*) FROM network_usage WHERE timestamp < datetime('now', '-%d months', 'localtime');",
                 opts->cleanup_months);
        snprintf(desc, sizeof(desc), "Keep last %d month(s) (purge records older than %d months)",
                 opts->cleanup_months, opts->cleanup_months);
    } else if (opts->cleanup_mode == CLEANUP_KEEP_RANGE) {
        snprintf(delete_sql, sizeof(delete_sql),
                 "DELETE FROM network_usage WHERE date(timestamp) < date('%s') OR date(timestamp) > date('%s'); VACUUM;",
                 opts->cleanup_start, opts->cleanup_end);
        snprintf(count_del_sql, sizeof(count_del_sql),
                 "SELECT COUNT(*) FROM network_usage WHERE date(timestamp) < date('%s') OR date(timestamp) > date('%s');",
                 opts->cleanup_start, opts->cleanup_end);
        snprintf(desc, sizeof(desc), "Keep records between %s and %s (purge all outside range)",
                 opts->cleanup_start, opts->cleanup_end);
    } else {
        return false;
    }

    long long total_records = 0;
    long long to_delete = 0;
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(database->db, "SELECT COUNT(*) FROM network_usage;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total_records = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(database->db, count_del_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            to_delete = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    long long to_keep = total_records - to_delete;
    if (to_keep < 0) to_keep = 0;

    printf("\n===================================================================================================\n");
    printf("                              ⚠️  DATABASE RETENTION & PURGE WARNING                               \n");
    printf("===================================================================================================\n");
    printf(" Target Database   : %s\n", database->db_path);
    printf(" Maintenance Plan  : %s\n", desc);
    printf(" Total Records     : %lld\n", total_records);
    printf(" Records to DELETE : \033[1;31m%lld\033[0m\n", to_delete);
    printf(" Records to KEEP   : \033[1;32m%lld\033[0m\n", to_keep);
    printf(" WARNING           : This action will permanently delete records and CANNOT be undone!\n");
    printf("===================================================================================================\n");

    if (!opts->skip_confirm) {
        printf(" Are you sure you want to proceed with permanent deletion? [y/N]: ");
        fflush(stdout);

        char answer[64] = "";
        if (fgets(answer, sizeof(answer), stdin) == NULL ||
            (answer[0] != 'y' && answer[0] != 'Y')) {
            printf("\n[*] Operation cancelled by user. No database records were modified.\n\n");
            return false;
        }
    }

    char *err_msg = NULL;
    int rc = sqlite3_exec(database->db, delete_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "\n[ERROR] Failed to execute database purge: %s\n\n", err_msg ? err_msg : "Unknown error");
        sqlite3_free(err_msg);
        return false;
    }

    printf("\n[✓] Successfully executed: %s\n", desc);
    printf("    Purged %lld records. Database successfully compacted with VACUUM.\n\n", to_delete);
    return true;
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

// ============================================================================
// Main Entrypoint
// ============================================================================
int main(int argc, char *argv[]) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    CliOptions opts = {
        .time_filter_active = false,
        .time_clause = "",
        .time_description = "",
        .min_bytes = 0,
        .sort_asc = false,
        .sort_specified = false,
        .group_by = GROUP_BY_APP,
        .run_daemon = false,
        .record_snapshot = false,
        .cleanup_mode = CLEANUP_NONE,
        .cleanup_days = 0,
        .cleanup_months = 0,
        .cleanup_start = "",
        .cleanup_end = "",
        .skip_confirm = false
    };

    enum {
        OPT_LAST_MINUTE = 1000,
        OPT_LAST_HOUR,
        OPT_MIN_SIZE,
        OPT_CLEAR,
        OPT_CGNAT_LOCAL,
        OPT_KEEP_DAYS,
        OPT_KEEP_MONTHS,
        OPT_KEEP_RANGE
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
        {"group-by",    required_argument, 0, 'g'},
        {"keep-days",   required_argument, 0, OPT_KEEP_DAYS},
        {"keep-months", required_argument, 0, OPT_KEEP_MONTHS},
        {"keep-range",  required_argument, 0, OPT_KEEP_RANGE},
        {"yes",         no_argument,       0, 'y'},
        {"force",       no_argument,       0, 'y'},
        {"cgnat-local", no_argument,       0, OPT_CGNAT_LOCAL},
        {"clear",       no_argument,       0, OPT_CLEAR},
        {"reset",       no_argument,       0, OPT_CLEAR},
        {"daemon",      no_argument,       0, 'D'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "clear") == 0 || strcmp(argv[i], "reset") == 0) {
            opts.cleanup_mode = CLEANUP_ALL;
            break;
        }
    }

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "d::w::m::c:s:g:yDh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'g':
                if (optarg) {
                    if (strcasecmp(optarg, "day") == 0 || strcasecmp(optarg, "daily") == 0) {
                        opts.group_by = GROUP_BY_DAY;
                    } else if (strcasecmp(optarg, "hour") == 0 || strcasecmp(optarg, "hourly") == 0) {
                        opts.group_by = GROUP_BY_HOUR;
                    } else if (strcasecmp(optarg, "week") == 0 || strcasecmp(optarg, "weekly") == 0) {
                        opts.group_by = GROUP_BY_WEEK;
                    } else if (strcasecmp(optarg, "month") == 0 || strcasecmp(optarg, "monthly") == 0) {
                        opts.group_by = GROUP_BY_MONTH;
                    } else if (strcasecmp(optarg, "app") == 0 || strcasecmp(optarg, "application") == 0) {
                        opts.group_by = GROUP_BY_APP;
                    } else {
                        fprintf(stderr, "Unknown group-by mode '%s'. Choose from: app, day, hour, week, month\n", optarg);
                        return 1;
                    }
                }
                break;

            case OPT_KEEP_DAYS:
                opts.cleanup_mode = CLEANUP_KEEP_DAYS;
                opts.cleanup_days = atoi(optarg);
                if (opts.cleanup_days <= 0) opts.cleanup_days = 1;
                break;

            case OPT_KEEP_MONTHS:
                opts.cleanup_mode = CLEANUP_KEEP_MONTHS;
                opts.cleanup_months = atoi(optarg);
                if (opts.cleanup_months <= 0) opts.cleanup_months = 1;
                break;

            case OPT_KEEP_RANGE: {
                opts.cleanup_mode = CLEANUP_KEEP_RANGE;
                char *comma = strchr(optarg, ',');
                if (!comma) comma = strchr(optarg, ':');
                if (comma) {
                    *comma = '\0';
                    strncpy(opts.cleanup_start, optarg, sizeof(opts.cleanup_start) - 1);
                    strncpy(opts.cleanup_end, comma + 1, sizeof(opts.cleanup_end) - 1);
                } else {
                    strncpy(opts.cleanup_start, optarg, sizeof(opts.cleanup_start) - 1);
                    if (optind < argc && argv[optind] && argv[optind][0] != '-') {
                        strncpy(opts.cleanup_end, argv[optind++], sizeof(opts.cleanup_end) - 1);
                    } else {
                        time_t t = time(NULL);
                        struct tm *tm_info = localtime(&t);
                        strftime(opts.cleanup_end, sizeof(opts.cleanup_end), "%Y-%m-%d", tm_info);
                    }
                }
                break;
            }

            case 'y':
                opts.skip_confirm = true;
                break;

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
                    opts.sort_specified = true;
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

            case OPT_CGNAT_LOCAL:
                config_treat_cgnat_as_local = true;
                break;

            case OPT_CLEAR:
                opts.cleanup_mode = CLEANUP_ALL;
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

    if (opts.cleanup_mode != CLEANUP_NONE) {
        Database db = {0};
        if (!init_database(&db, true)) {
            return 1;
        }
        purge_database_records(&db, &opts);
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
