# 🌐 NetMonitor (Linux Network Usage Monitor)

[![Language](https://img.shields.io/badge/Engine-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![GUI](https://img.shields.io/badge/GUI-PySide6%20%2F%20Qt%206-green.svg)](https://www.qt.io/)
[![Packet Capture](https://img.shields.io/badge/Capture-libpcap-red.svg)](https://www.tcpdump.org/)
[![Database](https://img.shields.io/badge/Database-SQLite3%20Amalgamation-lightgrey.svg)](https://www.sqlite.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%28Fedora%20%2F%20Ubuntu%20%2F%20Arch%29-orange.svg)](https://kernel.org)
[![Service](https://img.shields.io/badge/Service-Systemd%20Daemon-brightgreen.svg)](https://systemd.io/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A high-performance, real-time Linux network bandwidth monitor and background telemetry daemon. Unlike basic network utilities that merely estimate traffic, **NetMonitor uses direct packet capture via `libpcap`** (inspired by `nethogs`) to achieve maximum precision, featuring a **GlassWire-style 4-column breakdown** between external **Internet (WAN)** quota consumption and internal **Local Network (LAN)** sharing.

NetMonitor comes with **both a stunning Desktop GUI (Qt 6 / PySide6)** and a **fast, filterable CLI**.

---

<p align="center">
  <img src="assets/netmon.png" alt="NetMonitor App Icon" width="128" height="128">
</p>

---

## ⚡ Quick Start

### 1. Install System-Wide (CLI, Daemon & Desktop GUI)
Clone the repository and run `make install` with root privileges:

```bash
git clone https://github.com/ii3Bdallh/NetMonitor.git
cd NetMonitor
sudo make install
```

> **What this does automatically:**
> 1. Compiles the optimized `netmon` C binary with `libpcap` and embedded SQLite.
> 2. Installs `netmon` (CLI) and `netmon-gui` (Desktop App) to `/usr/local/bin/`.
> 3. Installs desktop icons and the application launcher (`NetMonitor.desktop`).
> 4. Configures and starts the background daemon `net_monitor.service` with `systemd` to log usage across reboots.

### 2. Launch the Desktop GUI
You can open NetMonitor via:
- **Desktop Shortcut**: Double-click the **NetMonitor** icon on your Desktop.
- **Application Menu**: Search for **NetMonitor** in your application launcher (KDE Kickoff, GNOME Activities, Rofi, etc.).
- **Terminal**: Run `netmon-gui` anywhere.

```bash
netmon-gui
```

### 3. Or Query from Terminal (CLI)
```bash
# View network usage for today (last 24 hours) with 4-column WAN/LAN breakdown
netmon --last-day

# View apps consuming 1 MB or more
netmon --last-day --min 1M

# See help & all available filters
netmon --help
```

---

## 🖥️ Desktop GUI Features (GlassWire-Inspired Edition)

The NetMonitor Desktop application features a sleek, dark glassmorphic interface inspired by GlassWire, designed for high-refresh Linux desktops with ultra-low CPU overhead:

- 📊 **GlassWire Multi-Column Cards View (with Interactive Drilldowns)**:
  - **Expandable Day Breakdown (`▶` / `▼`)**: Click any Day row or its cyan arrow (`▶`) to instantly expand the list of active applications for that specific day with their individual consumption, mini progress bars, and download/upload split. Click again (`▼`) to collapse.
  - **Dynamic Grouping Drilldowns**: When grouped by Day, expand days to see apps; when grouped by Application, expand apps to see their daily usage history; when grouped by Hour, expand hours to see running apps.
  - **Persistent State**: Open days/items remain expanded across periodic 3-second live auto-refreshes.
  - **WAN vs LAN Traffic**: Direct comparison with percentage distribution and dual-colored progress meters.
- 🎛️ **View Mode Switcher**:
  - `📊 Cards View`: Clean, high-level multi-column cards layout.
  - `📋 Data Table`: Complete 10-column data grid with in-place sorting and filtering.
- 🗂️ **Dynamic Group-By Aggregations**:
  - **Application**: Process-level breakdown.
  - **Day (Daily Usage)**: Track consumption per calendar day (e.g., today 5 GB, yesterday 6 GB, day before 7 GB).
  - **Hour (Hourly Usage)**: Hour-by-hour telemetry analysis.
  - **Week (Weekly Usage)**: Weekly aggregated usage.
  - **Month (Monthly Usage)**: Month-by-month historical quota tracking.
- 📉 **Dual-Colored Stacked Progress Bars**:
  - One dedicated bar for **WAN** (Internet) and one for **LAN** (Local).
  - Each bar features dual segments: **Download (Amber / Yellow)** and **Upload (Pink / Magenta)**.
- ⏱️ **Center Speedometer & Real-Time Throughput**:
  - Glowing circular arc gauge displaying total network consumption.
  - Real-time download rate (e.g., `23 KB/s ↓`) and upload rate (e.g., `1.4 MB/s ↑`).
- 📈 **Interactive Timeline Wave Chart**:
  - Dual smooth area waves: Download wave (Amber `#f59e0b`) and Upload wave (Pink `#ec4899`).
  - Interactive hover crosshair: hover cursor over any point to inspect timestamp, download, and upload totals.
  - **Time Window Toggle**: Instantly switch between **12 Hours**, **24 Hours**, **48 Hours**, and **72 Hours**.
- 📋 **Explicit Columns**:
  - Entity (App / Day / Hour / Week / Month)
  - **Total WAN Usage**
  - WAN Download (RX) & WAN Upload (TX)
  - **Total LAN Usage**
  - LAN Download (RX) & LAN Upload (TX)
  - Grand Total Usage
  - Batches / Samples & Last Seen
- 🕒 **Interactive Time Filters**: *Last 5 min, 15 min, 30 min, 1 Hour, 6 Hours, 24 Hours, 7 Days, 30 Days, All Time, Custom Date Picker*.
- 🔍 **Instant Search & Minimum Size Filters**: 150ms debounced search and quick thresholds ($\ge$ 100 KB, 1 MB, 10 MB, 100 MB, 1 GB).
- 💾 **CSV Export**: Export full 10-column reports to spreadsheet-ready CSV files with one click.
- 🗑️ **Database Retention & Purge Manager**: Selectively delete records outside a custom date range (keep start to end date), retain recent history (last 7, 10, 30, 60, 90 days), or completely reset data, featuring live projected impact counters (records to delete vs keep) and strict confirmation safeguards.

---

## 💻 CLI Output & Usage

### 10-Column Table Breakdown (with Total WAN & Total LAN)
```text
=====================================================================================================================================================
                                              DATABASE QUERY RESULTS                                                                                 
=====================================================================================================================================================
 Database: /var/lib/netmon/usage.db
 Group By: APPLICATION
 Filter  : Last 1 Day(s)
 Sort    : total_bytes DESC
-----------------------------------------------------------------------------------------------------------------------------------------------------
APPLICATION          TOTAL WAN     WAN RX       WAN TX       TOTAL LAN     LAN RX       LAN TX       TOTAL USAGE   SAMPLES   LAST SEEN          
-----------------------------------------------------------------------------------------------------------------------------------------------------
language_server      1.62 GB       239.39 MB    1.38 GB      1.44 GB       194.77 MB    1.26 GB      3.06 GB       2509      2026-10-06 18:41:49
brave                871.73 MB     722.14 MB    149.59 MB    1.73 GB       1.38 GB      360.22 MB    2.58 GB       3664      2026-10-06 18:41:49
antigravity-ide      265.46 MB     247.56 MB    17.90 MB     1.42 GB       960.37 MB    495.32 MB    1.68 GB       2475      2026-10-06 18:41:49
rclone               552.96 MB     370.19 MB    182.77 MB    0 B           0 B          0 B          552.96 MB     836       2026-10-05 22:24:45
code                 444.60 MB     438.08 MB    6.52 MB      657.36 KB     539.39 KB    117.97 KB    445.24 MB     43        2026-10-06 09:46:48
warp-terminal        236.62 MB     205.90 MB    30.72 MB     148.51 MB     127.94 MB    20.57 MB     385.12 MB     1178      2026-10-06 15:12:49
-----------------------------------------------------------------------------------------------------------------------------------------------------
TOTAL AGGREGATED     9.67 GB       7.54 GB      2.12 GB      5.54 GB       3.28 GB      2.26 GB      15.21 GB      Total Items: 57
-----------------------------------------------------------------------------------------------------------------------------------------------------
  🌐 Internet / WAN (Quota Usage)    : 9.67 GB    (Download: 7.54 GB   | Upload: 2.12 GB  )
  🏠 Local / LAN    (Network Sharing): 5.54 GB    (Download: 3.28 GB   | Upload: 2.26 GB  )
=====================================================================================================================================================
```

### CLI Options

#### 1. Group By Aggregations (`-g`, `--group-by`)
```bash
# Group consumption by Day (shows today, yesterday, previous days):
netmon --group-by day

# Group consumption by Hour over the last 24 hours:
netmon --group-by hour --last-day 1

# Group consumption by Week:
netmon --group-by week

# Group consumption by Month:
netmon --group-by month

# Group by Application (default):
netmon --group-by app
```

#### 2. Time Filters
```bash
# Usage in the last 30 minutes
netmon --last-minute 30

# Usage in the last 6 hours
netmon --last-hour 6

# Usage today (last 24 hours, default N=1)
netmon --last-day

# Usage over last 2 weeks (14 days)
netmon --last-week 2

# Usage over the last month
netmon --last-month 1

# Filter for a specific calendar date
netmon --custom "2026-10-05"
```

#### 3. Size Filters (`--min`)
```bash
# Only show apps with >= 500 KB total consumption
netmon --last-day --min 500K

# Only show heavy bandwidth consumers (>= 100 MB)
netmon --last-week --min 100M

# Show applications with >= 1 GB
netmon --min 1G
```

#### 4. Sorting (`-s`, `--sort`)
```bash
# Highest consumption first (default)
netmon --last-day --sort desc

# Lowest consumption first
netmon --last-day --sort asc
```

#### 5. CGNAT / Tailscale Configuration (`--cgnat-local`)
By default, CGNAT range `100.64.0.0/10` is classified as **Internet (WAN)** because most 4G/5G mobile carriers and ISPs use CGNAT for internet access. If you use a VPN mesh network like Tailscale on that range, pass:
```bash
netmon --cgnat-local
```

#### 6. Database Retention & Purge Manager
NetMonitor allows you to selectively purge records outside a date range, retain only recent history, or completely wipe the database:

```bash
# 1. Keep only records between two dates and delete everything outside this period:
sudo netmon --keep-range "2026-10-01" "2026-10-06"

# 2. Keep only the last 10 days and purge all older historical records:
sudo netmon --keep-days 10

# 3. Keep only the last 1 month (or 2 months) and purge older records:
sudo netmon --keep-months 1
sudo netmon --keep-months 2

# 4. Complete wipe (prompts for confirmation before deleting):
sudo netmon --clear

# 5. Skip interactive confirmation prompt (for automated scripts / cron jobs):
sudo netmon --keep-days 30 -y
sudo netmon --clear -y
```

---

## 🏗️ Technical Architecture & Pipeline

```text
Physical NICs (eth0, wlan0)
       │
       ▼
libpcap Capture Thread (Snaplen: 96B, BPF: "tcp or udp")
       │
       ▼
Thread-Safe Ring Buffer (Capacity: 65,536, Zero Allocations on Hot Path)
       │
       ▼
Collector Pipeline Loop (Batch Dequeue up to 4096 packets)
       ├── Scan /proc (Every 1s)    -> InodeMap + PID start_time validation
       ├── Scan /proc/net/*         -> O(1) 5-tuple Socket Hash Map
       └── Match Packet to Socket   -> Exact Wire Bytes (WAN/LAN Up/Down)
              │
              ▼ (Every 60s)
SQLite Database WAL Mode (/var/lib/netmon/usage.db)
       │
       ├── CLI Presentation (SQL Conditional Aggregation)
       └── Desktop GUI Application (PySide6 / Qt 6)
```

1. **libpcap Capture Layer**: Listens on all active physical interfaces (ignoring loopback `lo`, `docker0`, bridges `br-*`, and `veth*`). Uses a compact 96-byte snaplen to read headers without payload overhead, compiled with kernel BPF filter `"tcp or udp"`.
2. **Ring Buffer Queue**: Dedicated thread-safe queue holding 65,536 `PacketEvent` items. Capture threads acquire the mutex only for ~20 nanoseconds with zero dynamic heap allocation (`malloc`/`free`) on the packet hot path.
3. **$O(1)$ Socket Hash Table**: Rebuilt every 1 second as an in-memory lookup cache:
   - **Exact 5-tuple Table**: Instant match on `(proto, local_ip, local_port, remote_ip, remote_port)`.
   - **Wildcard Port Table**: Matches listening TCP and unconnected UDP sockets.
4. **PID Reuse Prevention**: Reads `starttime` (field 22) from `/proc/[PID]/stat`. If the PID was recycled by the kernel for another process, NetMonitor immediately invalidates cached metadata and re-reads the process command.
5. **Memory Cleanup (Pruning)**: Terminated processes not observed for 5 consecutive scanning cycles (5 seconds) are automatically evicted from memory.
6. **Accurate WAN vs LAN Detection**: Examines the actual remote IP on the captured packet against RFC 1918 private subnets, Link-Local, and ULA addresses.

---

## 🛠️ Prerequisites & Compilation

### Install Dependencies

#### Fedora / RHEL / CentOS:
```bash
sudo dnf install -y gcc make libpcap-devel python3-pyside6
```

#### Ubuntu / Debian:
```bash
sudo apt update
sudo apt install -y build-essential libpcap-dev python3-pyside6
```

#### Arch Linux:
```bash
sudo pacman -S --needed base-devel libpcap python-pyside6
```

---

### Build Commands

```bash
# 1. Compile the C binary locally
make

# 2. Test the desktop GUI locally
./netmon-gui

# 3. Install system-wide and start daemon
sudo make install

# 4. Uninstall completely from system
sudo make uninstall

# 5. Clean local build artifacts
make clean
```

---

## 🔧 Managing the Background Daemon

NetMonitor runs silently as a systemd service (`net_monitor.service`):

```bash
# Check daemon service status
systemctl status net_monitor.service

# View live background syslog entries
journalctl -u net_monitor.service -f

# Restart or stop the daemon
sudo systemctl restart net_monitor.service
sudo systemctl stop net_monitor.service
```

---

## 📂 Project Structure

```text
NetMonitor/
├── main.c                 # High-performance C packet capture & attribution daemon
├── netmon_gui.py          # Modern PySide6 (Qt 6) Desktop GUI application
├── netmon-gui             # Launcher shell script for Desktop GUI
├── NetMonitor.desktop     # Desktop Entry & application launcher specification
├── assets/
│   ├── netmon.png         # 256x256 application icon
│   ├── netmon_512.png     # 512x512 high-res application icon
│   └── netmon_64.png      # 64x64 application icon
├── sqlite3.c              # Embedded SQLite amalgamation source
├── sqlite3.h              # Embedded SQLite C API header
├── Makefile               # Automated build, install & packaging script
├── net_monitor.service    # Systemd daemon service unit
└── README.md              # Comprehensive documentation
```

---

## 👨‍💻 Author

Developed and maintained by **Abdallah Mamdouh** ([@ii3Bdallh](https://github.com/ii3Bdallh)).

---

## 📜 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
