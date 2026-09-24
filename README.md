# 🌐 NetMonitor (Linux Network Usage Monitor)

[![Language](https://img.shields.io/badge/Language-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Database](https://img.shields.io/badge/Database-SQLite3%20Amalgamation-lightgrey.svg)](https://www.sqlite.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%28Fedora%20%2F%20Ubuntu%20%2F%20Arch%29-orange.svg)](https://kernel.org)
[![Service](https://img.shields.io/badge/Service-Systemd%20Daemon-brightgreen.svg)](https://systemd.io/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A lightweight, zero-dependency, high-performance Linux network bandwidth monitoring tool and background daemon written in C. It continuously tracks per-process network consumption (Upload / Download) into an embedded SQLite database with atomic batch transactions, featuring **GlassWire-style breakdown** between external **Internet (WAN)** quota and internal **Local Network (LAN)** sharing.

---

## ⚡ Quick Start (For End-Users)

If you just want to install and use NetMonitor immediately on your Linux system, run this single command:

```bash
git clone https://github.com/ii3Bdallh/NetMonitor.git && cd NetMonitor && sudo make install
```

> **What this does automatically:**
> 1. Compiles the optimized binary.
> 2. Installs `netmon` globally to `/usr/local/bin/netmon`.
> 3. Configures and starts the background daemon `net_monitor.service` with `systemd` to log usage across reboots.

### 🎯 Start Querying Immediately
Once installed, run `netmon` from any terminal:

```bash
# View network usage for today (last 24 hours)
netmon --last-day

# View apps consuming 1 MB or more
netmon --last-day --min 1M

# See help & all available filters
netmon --help
```

---

## ✨ Features

- 🚀 **Zero External Dependencies**: Statically linked with the embedded SQLite amalgamation (`sqlite3.c`).
- 🔍 **Precise Per-Process Attribution**: Correlates active TCP/UDP socket descriptors in `/proc/net/tcp[6]` & `/proc/net/udp[6]` with `/proc/[PID]/fd` and per-process I/O metrics in `/proc/[PID]/io`.
- 🌐 **Internet (WAN) vs Local (LAN) Classification**: Accurately differentiates between external internet traffic (eating your quota) and local transfers (KDE Connect, local Wi-Fi sharing, SSHFS, Docker, LAN).
- ⚡ **Non-Blocking WAL Batch Commits**: Commits aggregated process bandwidth to SQLite every 60 seconds inside fast Write-Ahead Log (WAL) transactions without freezing tracking loops.
- 🕒 **Rich SQL-Driven CLI Filters**: Filter by minutes, hours, days, weeks, months, or custom calendar dates (`WHERE`, `HAVING`, `ORDER BY` executed inside SQL).
- 🧹 **Database Management**: Built-in `--clear` flag to easily purge historical records and vacuum storage.
- 🛡️ **Silent Background Daemon**: Production-ready double-fork daemon with `syslog` integration and `systemd` management.

---

## 🖥️ Live Output Preview

```text
===================================================================================================
                                   DATABASE QUERY RESULTS                                          
===================================================================================================
 Database: /var/lib/netmon/usage.db
 Filter  : Last 2 Day(s)
 Min Size: >= 1.00 MB
 Sort    : Total Bytes DESC
---------------------------------------------------------------------------------------------------
APPLICATION              UPLOAD (TX)      DOWNLOAD (RX)    TOTAL USAGE      SAMPLES    LAST SEEN           
---------------------------------------------------------------------------------------------------
ssh                      1.17 MB          95.09 MB         96.26 MB         2          2026-09-24 12:16:38 
antigravity-ide          118.90 KB        1.64 MB          1.76 MB          6          2026-09-24 12:16:38 
brave                    90.12 KB         1.57 MB          1.65 MB          6          2026-09-24 12:16:38 
---------------------------------------------------------------------------------------------------
TOTAL AGGREGATED         1.38 MB          98.29 MB         99.67 MB         Total Rows: 3
---------------------------------------------------------------------------------------------------
  🌐 Internet / WAN (Quota Usage) : 1.48 MB    (Upload: 343.44 KB | Download: 1.14 MB  )
  🏠 Local / LAN    (Network Sharing): 99.55 MB   (Upload: 1.36 MB   | Download: 98.18 MB )
===================================================================================================
```

---

## 🚀 CLI Usage & Filter Options

### 1. Time Filters
```bash
# Last 30 minutes
netmon --last-minute 30

# Last 6 hours
netmon --last-hour 6

# Today / Last 24 hours (or N days: netmon --last-day 5)
netmon --last-day

# Last 2 weeks
netmon --last-week 2

# Last month
netmon --last-month 1

# Specific custom date
netmon --custom "2026-09-24"
```

### 2. Size Filters (`--min`)
```bash
# Only show applications that consumed >= 500 KB
netmon --last-day --min 500K

# Show heavy consumers (>= 100 MB)
netmon --last-week --min 100M

# Show applications with >= 1 GB
netmon --min 1G
```

### 3. Sorting (`--sort`)
```bash
# Highest consumption first (default)
netmon --last-day --sort desc

# Lowest consumption first
netmon --last-day --sort asc
```

### 4. Reset / Clear Database History (`--clear`)
```bash
# Purge all historical usage records and vacuum the SQLite database
netmon --clear
```

---

## 🛠️ For Developers & Contributors

### Prerequisites
Make sure you have standard build tools installed:

```bash
# Fedora / RHEL
sudo dnf install gcc make

# Ubuntu / Debian
sudo apt install build-essential

# Arch Linux
sudo pacman -S base-devel
```

### Manual Compilation
```bash
# Build the binary locally
make

# Clean build artifacts
make clean

# Install or reinstall system-wide
sudo make install

# Completely uninstall from system
sudo make uninstall
```

### Project Structure
```text
NetMonitor/
├── main.c                 # Core engine (packet attribution, sockets, SQLite, CLI)
├── sqlite3.c              # SQLite 3.46.1 amalgamation source
├── sqlite3.h              # SQLite C API header
├── Makefile               # Automated build, install & uninstall scripts
├── net_monitor.service    # Systemd daemon service unit
└── README.md              # Documentation
```

---

## 🔧 Managing the Background Daemon

```bash
# Check daemon service status
sudo systemctl status net_monitor.service

# Stream live background logs
journalctl -u net_monitor.service -f

# Restart or Stop daemon
sudo systemctl restart net_monitor.service
sudo systemctl stop net_monitor.service
```

---

## 🏗️ Technical Architecture

1. **Active Hardware Interface**: Inspects `/proc/net/dev` to locate active network interfaces (Wi-Fi `wlp*`, Ethernet `eth*`, `enp*`) and measure hardware-level RX/TX delta rates.
2. **Socket Resolution & Subnet Parsing**: Scans `/proc/net/tcp[6]` and `/proc/net/udp[6]` to extract connection inodes and remote destination IPs. IPs are mapped to RFC 1918 subnets (`192.168.0.0/16`, `10.0.0.0/8`, `172.16.0.0/12`, Link-Local, IPv6 local, port 1716) to distinguish LAN from WAN.
3. **Process I/O Correlation**: Reads `/proc/[PID]/io` and `/proc/[PID]/fd/*` using `readlink()`, mapping kernel socket descriptors to specific running process names (`/proc/[PID]/comm`).
4. **SQLite WAL Persistence**: Database stored in `/var/lib/netmon/usage.db` (with user fallback) updated atomically in 60-second batch intervals for maximum disk write efficiency.

---

## 👨‍💻 Author

Developed and maintained by **Abdallah Mamdouh** ([@ii3Bdallh](https://github.com/ii3Bdallh)).

---

## 📜 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
