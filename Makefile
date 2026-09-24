CC = gcc
CFLAGS = -O2 -Wall -Wextra
LIBS = -lpthread -ldl -lm
TARGET = netmon
SERVICE_FILE = net_monitor.service
INSTALL_BIN = /usr/local/bin
INSTALL_SYSTEMD = /etc/systemd/system
DATA_DIR = /var/lib/netmon

all: $(TARGET)

sqlite3.o: sqlite3.c sqlite3.h
	@echo "[*] Compiling SQLite amalgamation (sqlite3.o)..."
	@$(CC) -O2 -w -c sqlite3.c -o sqlite3.o

$(TARGET): main.c sqlite3.o
	@echo "[*] Building $(TARGET)..."
	@$(CC) $(CFLAGS) main.c sqlite3.o -o $(TARGET) $(LIBS)
	@echo "[✓] Build complete: ./$(TARGET)"

install: $(TARGET)
	@echo "[*] Installing $(TARGET) to $(INSTALL_BIN)..."
	@install -m 755 $(TARGET) $(INSTALL_BIN)/$(TARGET)
	@echo "[*] Creating system data directory $(DATA_DIR)..."
	@install -d -m 777 $(DATA_DIR)
	@if [ -f usage.db ]; then cp -n usage.db $(DATA_DIR)/ 2>/dev/null || true; chmod 666 $(DATA_DIR)/usage.db* 2>/dev/null || true; fi
	@echo "[*] Installing systemd service..."
	@install -m 644 $(SERVICE_FILE) $(INSTALL_SYSTEMD)/$(SERVICE_FILE)
	@systemctl daemon-reload
	@systemctl restart $(SERVICE_FILE)
	@echo "[✓] NetMonitor installed and running globally!"

uninstall:
	@echo "[*] Stopping and disabling service..."
	@-systemctl stop $(SERVICE_FILE) 2>/dev/null || true
	@-systemctl disable $(SERVICE_FILE) 2>/dev/null || true
	@rm -f $(INSTALL_SYSTEMD)/$(SERVICE_FILE)
	@systemctl daemon-reload
	@echo "[*] Removing binary $(INSTALL_BIN)/$(TARGET)..."
	@rm -f $(INSTALL_BIN)/$(TARGET)
	@echo "[✓] NetMonitor uninstalled successfully."

clean:
	@rm -f $(TARGET) *.o

.PHONY: all install uninstall clean
