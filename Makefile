CC = gcc
CFLAGS = -O2 -Wall -Wextra
LIBS = -lpthread -ldl -lm
TARGET = netmon
SERVICE_FILE = net_monitor.service
INSTALL_BIN = /usr/local/bin
INSTALL_SYSTEMD = /etc/systemd/system

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
	@echo "[*] Installing systemd service..."
	@install -m 644 $(SERVICE_FILE) $(INSTALL_SYSTEMD)/$(SERVICE_FILE)
	@systemctl daemon-reload
	@systemctl enable --now $(SERVICE_FILE)
	@echo "[✓] NetMonitor installed and running as a background service!"

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
