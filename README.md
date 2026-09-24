# 🌐 NetMonitor (Linux Network Usage Monitor)

[![Language](https://img.shields.io/badge/Language-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Database](https://img.shields.io/badge/Database-SQLite3%20Amalgamation-lightgrey.svg)](https://www.sqlite.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%28Fedora%20%2F%20Ubuntu%20%2F%20Arch%29-orange.svg)](https://kernel.org)
[![Service](https://img.shields.io/badge/Service-Systemd%20Daemon-brightgreen.svg)](https://systemd.io/)

A lightweight, zero-dependency, high-performance Linux CLI tool and background daemon written in C. It continuously monitors and logs per-process network bandwidth consumption (download/upload) into an embedded SQLite database, offering flexible time and size-based SQL queries.

---

## ✨ Features

- 🚀 **Zero External Dependencies**: Statically linked with the embedded SQLite amalgamation (`sqlite3.c`).
- 🔍 **Per-Process Tracking**: Programmatically correlates active socket inodes in `/proc/net/tcp` and `/proc/net/udp` with process descriptors in `/proc/[PID]/fd` and process names in `/proc/[PID]/comm`.
- ⚡ **Ultra-Fast Batch Transactions**: Employs SQLite WAL mode (`Write-Ahead Logging`) for non-blocking sub-millisecond batch inserts every 60 seconds.
- 🌐 **Internet vs LAN Breakdown**: Distinguishes between external Internet (WAN) quota consumption and internal Local Network (LAN / Wi-Fi sharing / Docker) traffic.
- 🕒 **Flexible CLI Time Filters**: Query historical network usage by minutes, hours, days, weeks, months, or exact custom dates.
- 📊 **SQL-Driven Queries & Sorting**: All filtering, aggregation, and sorting are executed directly within SQL (`WHERE`, `HAVING`, `ORDER BY`).
- 🛡️ **Systemd Background Daemon**: Runs silently as a detached background service with double-fork daemonization and `syslog` integration.

---

## 📋 Requirements

Any standard Linux distribution (Fedora, Ubuntu, Debian, Arch Linux, etc.) with `gcc` and `make`:

```bash
# Fedora / RHEL
sudo dnf install gcc make

# Ubuntu / Debian
sudo apt install build-essential

# Arch Linux
sudo pacman -S base-devel
```

---

## ⚡ Quick Start (Build & Install)

### 1. Clone the Repository
```bash
git clone https://github.com/ii3Bdallh/NetMonitor.git
cd NetMonitor
```

### 2. Build the Project
```bash
make
```

### 3. Install & Start as a Background Service (One-Command)
```bash
sudo make install
```
> **What this does:**
> 1. Installs the binary to `/usr/local/bin/netmon`.
> 2. Configures and enables the systemd service `/etc/systemd/system/net_monitor.service`.
> 3. Starts background data collection immediately and across reboots.

---

## 🚀 CLI Usage & Query Examples

Once installed, you can run `netmon` from anywhere in your terminal:

### 1. Help & Options
```bash
netmon --help
```

### 2. Filter by Time
```bash
# Usage in the last 30 minutes
netmon --last-minute 30

# Usage in the last 6 hours
netmon --last-hour 6

# Usage today (last 24 hours) sorted descending
netmon --last-day --sort desc

# Usage in the last 7 days (or any N days, e.g., --last-day 5)
netmon --last-day 5

# Usage in the last 4 weeks
netmon --last-week 4

# Usage in the last 2 months
netmon --last-month 2

# Usage on a specific custom date
netmon --custom "2026-09-24"
```

### 3. Filter by Minimum Consumption (`--min`)
```bash
# Show applications that used 500 KB or more in the last 24 hours
netmon --last-day --min 500K

# Show heavy consumers (> 100 MB) in the last week
netmon --last-week --min 100M

# Show applications with >= 1 GB of traffic
netmon --min 1G
```

### 4. Sort Direction (`--sort`)
```bash
# Sort by highest traffic first (default)
netmon --last-day --sort desc

# Sort by lowest traffic first
netmon --last-day --sort asc
```

### 5. Clear Database History (`--clear` / `--reset`)
```bash
# Clear all historical usage records and vacuum database
netmon --clear
```

---

## 🖥️ Example Output

```text
===================================================================================================
                                   DATABASE QUERY RESULTS                                          
===================================================================================================
 Filter  : Last 1 Day(s)
 Sort    : Total Bytes DESC
---------------------------------------------------------------------------------------------------
APPLICATION              SENT (TX)        RECEIVED (RX)    TOTAL USAGE      SAMPLES    LAST SEEN           
---------------------------------------------------------------------------------------------------
kdeconnectd              21.02 KB         0 B              21.02 KB         7          2026-09-24 11:00:25 
rclone                   1.75 KB          0 B              1.75 KB          3          2026-09-24 10:58:25 
language_server          1.01 KB          0 B              1.01 KB          14         2026-09-24 11:00:25 
antigravity-ide          0 B              0 B              0 B              21         2026-09-24 11:00:25 
brave                    0 B              0 B              0 B              14         2026-09-24 11:00:25 
---------------------------------------------------------------------------------------------------
TOTAL AGGREGATED         23.78 KB         0 B              23.78 KB         Total Rows: 5
===================================================================================================
```

---

## 🔧 Managing the Background Service

```bash
# Check service status
sudo systemctl status net_monitor.service

# View live daemon logs
journalctl -u net_monitor.service -f

# Stop or restart the service
sudo systemctl stop net_monitor.service
sudo systemctl restart net_monitor.service

# Uninstall completely
sudo make uninstall
```

---

## 🏗️ Architecture & How It Works

1. **Active Interface Detection**: Reads `/proc/net/dev` to locate the main network interface (Wi-Fi/Ethernet) and capture hardware-level RX/TX traffic.
2. **Socket Resolution**: Parses `/proc/net/tcp[6]` and `/proc/net/udp[6]` to record active connection inodes and socket queues.
3. **PID Mapping**: Scans `/proc/[PID]/fd/*` using `readlink()` to map socket inodes to process IDs and reads `/proc/[PID]/comm` for application names without crashing if processes exit dynamically.
4. **SQLite WAL Storage**: Automatically provisions `usage.db` with indexed `network_usage` tables and commits periodic batches via atomic transactions.

---

## 👨‍💻 Author

Developed and maintained by **Abdallah Mamdouh** ([@ii3Bdallh](https://github.com/ii3Bdallh)).

---

## 📜 License

MIT License. Feel free to use, modify, and distribute.
