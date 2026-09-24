#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>

#define PROC_NET_DEV "/proc/net/dev"
#define BUFFER_SIZE 512

static volatile bool keep_running = true;

void handle_sigint(int sig) {
    (void)sig;
    keep_running = false;
}

typedef struct {
    char name[32];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} NetInterface;

bool get_active_interface_stats(NetInterface *iface) {
    FILE *file = fopen(PROC_NET_DEV, "r");
    if (!file) {
        perror("Error opening " PROC_NET_DEV);
        return false;
    }

    char line[BUFFER_SIZE];
    bool found = false;
    unsigned long long max_traffic = 0;

    // Skip the first two header lines
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

        // Ignore loopback
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

int main(void) {
    signal(SIGINT, handle_sigint);

    printf("====================================================\n");
    printf("     Network Usage Monitor - Stage 1 (Total Usage)  \n");
    printf("====================================================\n");
    printf("Press Ctrl+C to stop.\n\n");

    NetInterface active_iface;

    while (keep_running) {
        if (get_active_interface_stats(&active_iface)) {
            double rx_mb = (double)active_iface.rx_bytes / (1024.0 * 1024.0);
            double tx_mb = (double)active_iface.tx_bytes / (1024.0 * 1024.0);
            double total_mb = rx_mb + tx_mb;

            printf("\r[Interface: %-8s] RX: %8.2f MB | TX: %8.2f MB | Total: %8.2f MB",
                   active_iface.name, rx_mb, tx_mb, total_mb);
            fflush(stdout);
        } else {
            printf("\r[Warning] No active network interface detected...");
            fflush(stdout);
        }

        sleep(1);
    }

    printf("\n\nMonitoring stopped successfully.\n");
    return 0;
}
