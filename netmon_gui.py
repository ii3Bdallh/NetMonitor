#!/usr/bin/env python3
"""
NetMonitor Desktop Application (GUI) - GlassWire-Inspired Edition
A fast, asynchronous, multi-threaded Linux desktop GUI for NetMonitor built with PySide6 (Qt 6).

Key Features:
- GlassWire Aesthetics: Arc gauge speedometer, dual-colored stacked progress bars (Download=Amber, Upload=Pink).
- Interactive 12h/24h/48h/72h Timeline Wave Graph with dual-layer curves, hover crosshair, and tooltips.
- Group By Support: Group by Application, Day (Today, Yesterday, etc.), Hour, Week, Month.
- Complete Column Breakdown: Includes explicit Total WAN Usage and Total LAN Usage columns.
- View Switcher: GlassWire Multi-Column Cards View vs Detailed Data Table View.
- Ultra-Low CPU: Background SQLite threading, in-place widget updates, sub-millisecond /proc daemon detection.
"""

import sys
import os
import sqlite3
import csv
import time
from datetime import datetime, timedelta

from PySide6 import QtCore, QtGui, QtWidgets
from PySide6.QtCore import Qt, QTimer, QThread, Signal, Slot, QPointF, QRectF
from PySide6.QtGui import (
    QIcon, QFont, QColor, QPainter, QPainterPath, QPen, QBrush,
    QLinearGradient, QPolygonF
)
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QLineEdit, QTableWidget,
    QTableWidgetItem, QHeaderView, QFrame, QFileDialog, QMessageBox,
    QDateEdit, QCheckBox, QStackedWidget, QScrollArea, QMenu, QToolButton,
    QButtonGroup, QDialog, QRadioButton, QGroupBox, QSpinBox
)

DEFAULT_SYSTEM_DB = "/var/lib/netmon/usage.db"

def get_db_path():
    if os.path.exists(DEFAULT_SYSTEM_DB) and os.access(DEFAULT_SYSTEM_DB, os.R_OK):
        return DEFAULT_SYSTEM_DB
    home = os.environ.get("HOME")
    if home:
        user_db = os.path.join(home, ".local/share/netmon/usage.db")
        if os.path.exists(user_db):
            return user_db
    return DEFAULT_SYSTEM_DB

def is_daemon_running_fast():
    """Non-blocking procfs check to test if netmon daemon is active without forking subprocess."""
    try:
        with os.scandir('/proc') as entries:
            for entry in entries:
                if entry.name.isdigit():
                    try:
                        with open(f"/proc/{entry.name}/comm", "r") as f:
                            if f.read().strip() == "netmon":
                                return True
                    except (OSError, IOError):
                        continue
    except Exception:
        pass
    return False

def format_bytes(b):
    if b is None or b < 0:
        return "0 B"
    if b >= 1024 ** 4:
        return f"{b / (1024 ** 4):.2f} TB"
    if b >= 1024 ** 3:
        return f"{b / (1024 ** 3):.2f} GB"
    if b >= 1024 ** 2:
        return f"{b / (1024 ** 2):.2f} MB"
    if b >= 1024:
        return f"{b / 1024:.2f} KB"
    return f"{int(b)} B"

def format_speed(bps):
    if bps is None or bps < 0:
        return "0 B/s"
    if bps >= 1024 ** 3:
        return f"{bps / (1024 ** 3):.1f} GB/s"
    if bps >= 1024 ** 2:
        return f"{bps / (1024 ** 2):.1f} MB/s"
    if bps >= 1024:
        return f"{bps / 1024:.1f} KB/s"
    return f"{int(bps)} B/s"


class NumericTableWidgetItem(QTableWidgetItem):
    """Custom QTableWidgetItem for accurate numeric/byte sorting."""
    def __init__(self, text, sort_val):
        super().__init__(text)
        self.sort_val = sort_val

    def __lt__(self, other):
        if isinstance(other, NumericTableWidgetItem):
            return self.sort_val < other.sort_val
        return super().__lt__(other)


# ============================================================================
# Custom GlassWire UI Widgets
# ============================================================================

class DualProgressBar(QWidget):
    """
    GlassWire-style stacked dual progress bar:
    - Amber/Yellow (#f59e0b) for Download segment.
    - Pink/Magenta (#ec4899) for Upload segment.
    - Background track (#1e293b).
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.download_val = 0
        self.upload_val = 0
        self.max_val = 1
        self.setFixedHeight(12)
        self.setMinimumWidth(80)
        self.setSizePolicy(QtWidgets.QSizePolicy.Policy.Expanding, QtWidgets.QSizePolicy.Policy.Fixed)

    def set_values(self, download_val, upload_val, max_val):
        self.download_val = max(0, download_val)
        self.upload_val = max(0, upload_val)
        self.max_val = max(1, max_val)
        tot = self.download_val + self.upload_val
        self.setToolTip(
            f"Total: {format_bytes(tot)}\n"
            f"↓ Download: {format_bytes(self.download_val)}\n"
            f"↑ Upload: {format_bytes(self.upload_val)}"
        )
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w = self.width()
        h = self.height()
        radius = h / 2.0

        # 1. Background Track
        track_path = QPainterPath()
        track_path.addRoundedRect(0, 0, w, h, radius, radius)
        painter.fillPath(track_path, QColor("#1e293b"))
        painter.strokePath(track_path, QPen(QColor("#334155"), 1.0))

        tot = self.download_val + self.upload_val
        if tot <= 0:
            return

        # 2. Filled Segment Ratio (proportional to max_val)
        ratio = min(1.0, tot / float(self.max_val))
        active_w = max(6.0, w * ratio)

        down_ratio = self.download_val / float(tot) if tot > 0 else 0.5
        down_w = active_w * down_ratio
        up_w = active_w - down_w

        painter.save()
        painter.setClipPath(track_path)

        # Download Portion (Amber / Yellow)
        if down_w > 0:
            down_grad = QLinearGradient(0, 0, down_w, 0)
            down_grad.setColorAt(0.0, QColor("#fbbf24"))
            down_grad.setColorAt(1.0, QColor("#f59e0b"))
            painter.fillRect(QRectF(0, 0, down_w, h), QBrush(down_grad))

        # Upload Portion (Pink / Magenta)
        if up_w > 0:
            up_grad = QLinearGradient(down_w, 0, down_w + up_w, 0)
            up_grad.setColorAt(0.0, QColor("#f43f5e"))
            up_grad.setColorAt(1.0, QColor("#ec4899"))
            painter.fillRect(QRectF(down_w, 0, up_w, h), QBrush(up_grad))

        painter.restore()


class SpeedometerArc(QWidget):
    """
    GlassWire-style center arc gauge with a vibrant yellow/gold circular ring
    and prominent center total data usage value.
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.total_str = "0 B"
        self.percent = 0.65
        self.setFixedSize(130, 75)

    def set_total(self, total_str, percent=0.75):
        self.total_str = total_str
        self.percent = max(0.05, min(1.0, percent))
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w = self.width()
        h = self.height()

        arc_rect = QRectF(15, 8, w - 30, (h - 12) * 2)

        # Base Track Arc (Grey/Slate)
        pen_bg = QPen(QColor("#1e293b"), 6)
        pen_bg.setCapStyle(Qt.PenCapStyle.RoundCap)
        painter.setPen(pen_bg)
        # Span from 140 to 40 degrees (260 degree arc)
        painter.drawArc(arc_rect, 20 * 16, 140 * 16)

        # Active Yellow/Gold Arc
        active_span = int(140 * self.percent * 16)
        pen_active = QPen(QColor("#fbbf24"), 6)
        pen_active.setCapStyle(Qt.PenCapStyle.RoundCap)
        painter.setPen(pen_active)
        painter.drawArc(arc_rect, (160 - int(140 * self.percent)) * 16, active_span)

        # Center Bell/Gauge Icon
        painter.setPen(QColor("#94a3b8"))
        painter.setFont(QFont("sans-serif", 9))
        painter.drawText(QRectF(0, 18, w, 14), Qt.AlignmentFlag.AlignCenter, "🔔")

        # Center Number
        painter.setPen(QColor("#f8fafc"))
        painter.setFont(QFont("sans-serif", 12, QFont.Weight.Bold))
        painter.drawText(QRectF(0, 36, w, 22), Qt.AlignmentFlag.AlignCenter, self.total_str)

        # Subtitle
        painter.setPen(QColor("#64748b"))
        painter.setFont(QFont("sans-serif", 8, QFont.Weight.Medium))
        painter.drawText(QRectF(0, 58, w, 14), Qt.AlignmentFlag.AlignCenter, "AGGREGATED")


class TimelineWaveChart(QWidget):
    """
    GlassWire-style interactive Timeline Wave Graph.
    Features:
    - Amber/Yellow wave for Download (RX) with subtle gradient fill.
    - Pink/Magenta wave for Upload (TX) with subtle gradient fill.
    - Interactive hover tracker: crosshair line + floating badge tooltip.
    - Scrubber bar and handles at bottom.
    - 12h, 24h, 48h, 72h window support.
    """
    range_changed = Signal(int) # Emits hours (12, 24, 48, 72)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedHeight(120)
        self.setMouseTracking(True)
        self.points = [] # [(datetime, rx, tx), ...]
        self.start_dt = datetime.now() - timedelta(hours=24)
        self.end_dt = datetime.now()
        self.current_hours = 24
        self.hover_x = -1
        self.hover_point = None

    def set_data(self, points, hours=24):
        self.current_hours = hours
        self.end_dt = datetime.now()
        self.start_dt = self.end_dt - timedelta(hours=hours)
        self.points = points
        self.update()

    def mouseMoveEvent(self, event):
        x = event.position().x()
        self.hover_x = x
        self.hover_point = self.get_point_at_x(x)
        self.update()

    def leaveEvent(self, event):
        self.hover_x = -1
        self.hover_point = None
        self.update()

    def get_point_at_x(self, x):
        if not self.points:
            return None
        w = float(self.width())
        if w <= 0:
            return None
        total_sec = (self.end_dt - self.start_dt).total_seconds()
        target_dt = self.start_dt + timedelta(seconds=(x / w) * total_sec)

        # Find closest point
        closest = min(self.points, key=lambda p: abs((p[0] - target_dt).total_seconds()))
        return closest

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w = float(self.width())
        h = float(self.height())
        chart_h = h - 22.0 # Reserve bottom for time axis & scrubber

        # Background Container
        bg_rect = QRectF(0, 0, w, h)
        painter.fillRect(bg_rect, QColor("#0d131f"))

        # Subtle Horizontal Grid Lines (25%, 50%, 75%)
        grid_pen = QPen(QColor("#1e293b"), 1, Qt.PenStyle.DashLine)
        painter.setPen(grid_pen)
        for pct in [0.25, 0.50, 0.75]:
            y = chart_h * pct
            painter.drawLine(QPointF(0, y), QPointF(w, y))

        # Bottom Timeline Scrubber Line
        painter.setPen(QPen(QColor("#334155"), 1))
        painter.drawLine(QPointF(0, chart_h), QPointF(w, chart_h))

        # Time Axis Tick Labels
        painter.setPen(QColor("#64748b"))
        painter.setFont(QFont("sans-serif", 8))
        num_ticks = 5
        total_sec = (self.end_dt - self.start_dt).total_seconds()
        for i in range(num_ticks):
            frac = i / float(num_ticks - 1) if num_ticks > 1 else 0
            tick_dt = self.start_dt + timedelta(seconds=frac * total_sec)
            tick_x = frac * w
            tick_text = tick_dt.strftime("%I:%M %p" if self.current_hours <= 24 else "%m/%d %H:%M")
            text_rect = QRectF(max(0, tick_x - 40), chart_h + 3, 80, 16)
            align = Qt.AlignmentFlag.AlignCenter
            if i == 0:
                align = Qt.AlignmentFlag.AlignLeft
                text_rect.setLeft(4)
            elif i == num_ticks - 1:
                align = Qt.AlignmentFlag.AlignRight
                text_rect.setRight(w - 4)
            painter.drawText(text_rect, align, tick_text)

        if not self.points:
            painter.setPen(QColor("#475569"))
            painter.setFont(QFont("sans-serif", 10))
            painter.drawText(QRectF(0, 0, w, chart_h), Qt.AlignmentFlag.AlignCenter, "No telemetry records in selected window.")
            return

        # Calculate Scales
        max_val = max(max((p[1] for p in self.points), default=1), max((p[2] for p in self.points), default=1))
        if max_val <= 0:
            max_val = 1
        max_val *= 1.15 # 15% headroom

        def get_coords(dt, val):
            sec_offset = (dt - self.start_dt).total_seconds()
            x = (sec_offset / total_sec) * w
            y = chart_h - (float(val) / max_val * (chart_h - 10))
            return max(0.0, min(w, x)), max(8.0, min(chart_h, y))

        sorted_pts = sorted(self.points, key=lambda p: p[0])

        # --- 1. Draw Download Wave (Amber / Yellow) ---
        rx_path = QPainterPath()
        rx_poly = [QPointF(0, chart_h)]
        for idx, p in enumerate(sorted_pts):
            px, py = get_coords(p[0], p[1])
            if idx == 0:
                rx_poly.append(QPointF(0, py))
                rx_path.moveTo(0, py)
            rx_poly.append(QPointF(px, py))
            rx_path.lineTo(px, py)
        rx_poly.append(QPointF(w, chart_h))

        # Filled Gradient Under Download Wave
        rx_fill_path = QPainterPath()
        rx_fill_path.addPolygon(QPolygonF(rx_poly))
        rx_fill_path.closeSubpath()

        rx_grad = QLinearGradient(0, 0, 0, chart_h)
        rx_grad.setColorAt(0.0, QColor(245, 158, 11, 140))
        rx_grad.setColorAt(1.0, QColor(245, 158, 11, 15))
        painter.fillPath(rx_fill_path, QBrush(rx_grad))

        # Stroke Line
        painter.strokePath(rx_path, QPen(QColor("#fbbf24"), 2.0))

        # --- 2. Draw Upload Wave (Pink / Rose) ---
        tx_path = QPainterPath()
        tx_poly = [QPointF(0, chart_h)]
        for idx, p in enumerate(sorted_pts):
            px, py = get_coords(p[0], p[2])
            if idx == 0:
                tx_poly.append(QPointF(0, py))
                tx_path.moveTo(0, py)
            tx_poly.append(QPointF(px, py))
            tx_path.lineTo(px, py)
        tx_poly.append(QPointF(w, chart_h))

        # Filled Gradient Under Upload Wave
        tx_fill_path = QPainterPath()
        tx_fill_path.addPolygon(QPolygonF(tx_poly))
        tx_fill_path.closeSubpath()

        tx_grad = QLinearGradient(0, 0, 0, chart_h)
        tx_grad.setColorAt(0.0, QColor(236, 72, 153, 140))
        tx_grad.setColorAt(1.0, QColor(236, 72, 153, 15))
        painter.fillPath(tx_fill_path, QBrush(tx_grad))

        # Stroke Line
        painter.strokePath(tx_path, QPen(QColor("#ec4899"), 2.0))

        # --- 3. Interactive Hover Crosshair & Tooltip ---
        if self.hover_x >= 0 and self.hover_point:
            h_dt, h_rx, h_tx = self.hover_point
            hx, hy_rx = get_coords(h_dt, h_rx)
            _, hy_tx = get_coords(h_dt, h_tx)

            # Vertical guide line
            painter.setPen(QPen(QColor("#94a3b8"), 1, Qt.PenStyle.DashLine))
            painter.drawLine(QPointF(hx, 0), QPointF(hx, chart_h))

            # Point dots
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#fbbf24"))
            painter.drawEllipse(QPointF(hx, hy_rx), 4, 4)
            painter.setBrush(QColor("#ec4899"))
            painter.drawEllipse(QPointF(hx, hy_tx), 4, 4)

            # Tooltip Badge Box
            tip_w = 160
            tip_h = 58
            tip_x = hx + 12 if (hx + tip_w + 12) < w else hx - tip_w - 12
            tip_y = 6

            tip_rect = QRectF(tip_x, tip_y, tip_w, tip_h)
            tip_path = QPainterPath()
            tip_path.addRoundedRect(tip_rect, 6, 6)
            painter.fillPath(tip_path, QColor("#1e293b"))
            painter.strokePath(tip_path, QPen(QColor("#38bdf8"), 1))

            painter.setPen(QColor("#cbd5e1"))
            painter.setFont(QFont("sans-serif", 8, QFont.Weight.Bold))
            painter.drawText(QRectF(tip_x + 8, tip_y + 4, tip_w - 16, 14), Qt.AlignmentFlag.AlignLeft, h_dt.strftime("%Y-%m-%d %H:%M"))

            painter.setFont(QFont("sans-serif", 8))
            painter.setPen(QColor("#fbbf24"))
            painter.drawText(QRectF(tip_x + 8, tip_y + 20, tip_w - 16, 14), Qt.AlignmentFlag.AlignLeft, f"↓ Down: {format_bytes(h_rx)}")

            painter.setPen(QColor("#ec4899"))
            painter.drawText(QRectF(tip_x + 8, tip_y + 36, tip_w - 16, 14), Qt.AlignmentFlag.AlignLeft, f"↑ Up:   {format_bytes(h_tx)}")


class ChildAppRow(QWidget):
    """A lightweight, reusable child row widget for an application or period."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        box = QVBoxLayout(self)
        box.setContentsMargins(2, 1, 2, 1)
        box.setSpacing(2)

        top = QHBoxLayout()
        top.setSpacing(4)
        self.lbl_name = QLabel()
        self.lbl_name.setStyleSheet("color: #e2e8f0; font-size: 10.5px; font-weight: 600; border: none;")
        top.addWidget(self.lbl_name, stretch=1)

        self.lbl_val = QLabel()
        self.lbl_val.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.lbl_val.setStyleSheet("color: #f8fafc; font-size: 10.5px; font-weight: bold; border: none;")
        top.addWidget(self.lbl_val)
        box.addLayout(top)

        self.bar = DualProgressBar()
        self.bar.setFixedHeight(5)
        box.addWidget(self.bar)

        self.lbl_stats = QLabel()
        self.lbl_stats.setStyleSheet("color: #64748b; font-size: 9.5px; border: none;")
        box.addWidget(self.lbl_stats)

    def update_data(self, child, parent_tot):
        c_name = child.get("name", "[unknown]")
        self.lbl_name.setText(f"📦 {c_name}" if not c_name.startswith("📅") else c_name)
        self.lbl_val.setText(format_bytes(child.get("total", 0)))

        down_b = child.get("wan_down", 0) + child.get("lan_down", 0)
        up_b = child.get("wan_up", 0) + child.get("lan_up", 0)
        self.bar.set_values(down_b, up_b, parent_tot)
        self.lbl_stats.setText(f"↓ {format_bytes(down_b)}  |  ↑ {format_bytes(up_b)}")


class ExpandableEntityCardItem(QWidget):
    """
    GlassWire-style Expandable Card Row representing an entity (e.g. Day, App, Hour).
    Zero-lag architecture with in-place updates, persistent expansion, and lazy sub-panel rendering.
    """
    toggled = Signal(str, bool)           # (raw_key, is_expanded)
    show_all_toggled = Signal(str, bool)  # (raw_key, show_all)

    def __init__(self, item, max_parent_tot, children=None, is_expanded=False, show_all=False, parent=None):
        super().__init__(parent)
        self.item = item
        self.raw_key = item.get("raw_entity", item.get("entity", ""))
        self.children = children or []
        self.is_expanded = is_expanded
        self.show_all_children = show_all
        self.max_parent_tot = max(1, max_parent_tot)
        self.child_rows = []

        self._setup_ui()

    def _setup_ui(self):
        root_layout = QVBoxLayout(self)
        root_layout.setContentsMargins(0, 1, 0, 1)
        root_layout.setSpacing(0)

        # 1. Header Frame (Clickable)
        self.header_frame = QFrame()
        self.header_frame.setObjectName("HeaderFrame")
        self.header_frame.setCursor(Qt.CursorShape.PointingHandCursor)
        self.header_frame.setStyleSheet("""
            QFrame#HeaderFrame {
                background-color: #121927;
                border: 1px solid #1e293b;
                border-radius: 6px;
                padding: 3px 6px;
            }
            QFrame#HeaderFrame:hover {
                background-color: #1a2538;
                border: 1px solid #38bdf8;
            }
        """)
        self.header_frame.mousePressEvent = self._on_header_clicked

        h_box = QVBoxLayout(self.header_frame)
        h_box.setContentsMargins(3, 3, 3, 3)
        h_box.setSpacing(3)

        top_row = QHBoxLayout()
        top_row.setSpacing(6)

        # Arrow indicator
        self.arrow_lbl = QLabel("▼" if self.is_expanded else "▶")
        self.arrow_lbl.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.arrow_lbl.setStyleSheet("color: #38bdf8; font-size: 11px; font-weight: bold; border: none; background: transparent;")
        top_row.addWidget(self.arrow_lbl)

        # Entity Title
        self.lbl_name = QLabel(self.item.get("entity", ""))
        self.lbl_name.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.lbl_name.setStyleSheet("color: #f1f5f9; font-size: 11px; font-weight: 600; border: none; background: transparent;")
        top_row.addWidget(self.lbl_name, stretch=1)

        # Count badge
        self.badge_lbl = QLabel(f"{len(self.children)}" if self.children else "")
        self.badge_lbl.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.badge_lbl.setVisible(bool(self.children))
        self.badge_lbl.setStyleSheet("""
            color: #94a3b8;
            font-size: 9px;
            font-weight: 600;
            background-color: #1e293b;
            border: 1px solid #334155;
            border-radius: 3px;
            padding: 0px 4px;
        """)
        top_row.addWidget(self.badge_lbl)

        # Total Bytes
        tot_val = self.item.get("total", 0)
        self.lbl_val = QLabel(format_bytes(tot_val))
        self.lbl_val.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.lbl_val.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.lbl_val.setStyleSheet("color: #38bdf8; font-size: 11px; font-weight: 700; border: none; background: transparent;")
        top_row.addWidget(self.lbl_val)

        h_box.addLayout(top_row)

        # Dual Progress Bar
        self.bar = DualProgressBar()
        self.bar.setFixedHeight(8)
        self.bar.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.bar.set_values(
            self.item.get("wan_down", 0) + self.item.get("lan_down", 0),
            self.item.get("wan_up", 0) + self.item.get("lan_up", 0),
            self.max_parent_tot
        )
        h_box.addWidget(self.bar)

        root_layout.addWidget(self.header_frame)

        # 2. Sub-Panel
        self.sub_panel = QFrame()
        self.sub_panel.setObjectName("SubPanel")
        self.sub_panel.setStyleSheet("""
            QFrame#SubPanel {
                background-color: #0b111e;
                border-left: 2px solid #38bdf8;
                border-bottom: 1px solid #1e293b;
                border-right: 1px solid #1e293b;
                border-radius: 0 0 6px 6px;
                margin-left: 6px;
                margin-right: 0px;
                margin-top: 1px;
                padding: 4px 6px 6px 8px;
            }
        """)
        self.sub_layout = QVBoxLayout(self.sub_panel)
        self.sub_layout.setContentsMargins(4, 4, 4, 4)
        self.sub_layout.setSpacing(4)

        # Sub-header label
        self.sub_hdr = QLabel()
        self.sub_hdr.setStyleSheet("color: #7dd3fc; font-size: 10px; font-weight: bold; border: none; padding-bottom: 2px; background: transparent;")
        self.sub_layout.addWidget(self.sub_hdr)

        # No items label
        self.no_items_lbl = QLabel("No application activity recorded for this period.")
        self.no_items_lbl.setStyleSheet("color: #64748b; font-size: 10px; font-style: italic; border: none; background: transparent;")
        self.sub_layout.addWidget(self.no_items_lbl)

        # Show more / Show less button
        self.more_btn = QPushButton()
        self.more_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.more_btn.setStyleSheet("""
            QPushButton {
                background-color: #162032;
                color: #38bdf8;
                border: 1px dashed #334155;
                border-radius: 4px;
                font-size: 10px;
                font-weight: 600;
                padding: 3px 6px;
                margin-top: 2px;
            }
            QPushButton:hover {
                background-color: #1e293b;
                border-color: #38bdf8;
                color: #7dd3fc;
            }
        """)
        self.more_btn.clicked.connect(self._toggle_show_all)
        self.sub_layout.addWidget(self.more_btn)

        self.sub_panel.setVisible(self.is_expanded)
        root_layout.addWidget(self.sub_panel)

        if self.is_expanded:
            self._sync_sub_panel()

        self._update_tooltip()

    def _on_header_clicked(self, event):
        self.toggle_expanded()

    def toggle_expanded(self):
        self.is_expanded = not self.is_expanded
        self.arrow_lbl.setText("▼" if self.is_expanded else "▶")
        self.sub_panel.setVisible(self.is_expanded)
        if self.is_expanded:
            self._sync_sub_panel()
        self._update_tooltip()
        self.toggled.emit(self.raw_key, self.is_expanded)

    def _toggle_show_all(self):
        self.show_all_children = not self.show_all_children
        self.show_all_toggled.emit(self.raw_key, self.show_all_children)
        self._sync_sub_panel()

    def _update_tooltip(self):
        if self.is_expanded:
            self.header_frame.setToolTip("Click to collapse applications list (▼)")
        else:
            self.header_frame.setToolTip(f"Click to expand applications for this item (▶) - {len(self.children)} items")

    def _sync_sub_panel(self):
        if not self.children:
            self.sub_hdr.setVisible(False)
            self.no_items_lbl.setVisible(True)
            self.more_btn.setVisible(False)
            for row in self.child_rows:
                row.setVisible(False)
            return

        self.no_items_lbl.setVisible(False)
        self.sub_hdr.setVisible(True)

        child_type_lbl = "Active Applications" if ("📅" in self.item.get("entity", "") or "day" in self.raw_key.lower()) else "Breakdown"
        self.sub_hdr.setText(f"{child_type_lbl} ({len(self.children)} items):")

        parent_tot = max(1, self.item.get("total", 1))
        limit = len(self.children) if self.show_all_children else min(8, len(self.children))

        # Dynamically reuse or create ChildAppRow widgets (lazy: only create what is needed)
        while len(self.child_rows) < limit:
            row_w = ChildAppRow()
            # Insert right before the more_btn (which is the last item in sub_layout)
            self.sub_layout.insertWidget(self.sub_layout.count() - 1, row_w)
            self.child_rows.append(row_w)

        # Update data and set visibility
        for i in range(limit):
            row_w = self.child_rows[i]
            row_w.update_data(self.children[i], parent_tot)
            row_w.setVisible(True)

        # Hide any unused rows beyond limit
        for i in range(limit, len(self.child_rows)):
            self.child_rows[i].setVisible(False)

        # Update Show More / Show Less button
        if len(self.children) > 8:
            self.more_btn.setVisible(True)
            if self.show_all_children:
                self.more_btn.setText("▲ Show less (top 8 only)")
            else:
                self.more_btn.setText(f"▶ Show all {len(self.children)} items (+{len(self.children) - 8} more)")
        else:
            self.more_btn.setVisible(False)

    def update_data(self, item, max_parent_tot, children, is_expanded, show_all):
        """High-performance in-place update without destroying widgets or moving scroll position."""
        self.item = item
        self.children = children or []
        self.max_parent_tot = max(1, max_parent_tot)
        self.is_expanded = is_expanded
        self.show_all_children = show_all

        # Update header in-place
        self.arrow_lbl.setText("▼" if self.is_expanded else "▶")
        self.lbl_name.setText(item.get("entity", ""))

        if self.children:
            self.badge_lbl.setText(f"{len(self.children)}")
            self.badge_lbl.setVisible(True)
        else:
            self.badge_lbl.setVisible(False)

        self.lbl_val.setText(format_bytes(item.get("total", 0)))
        self.bar.set_values(
            item.get("wan_down", 0) + item.get("lan_down", 0),
            item.get("wan_up", 0) + item.get("lan_up", 0),
            self.max_parent_tot
        )

        self.sub_panel.setVisible(self.is_expanded)
        if self.is_expanded:
            self._sync_sub_panel()

        self._update_tooltip()


class GlassWireCardsWidget(QWidget):
    """
    GlassWire-style 4-column card view layout with interactive drilldowns:
    - Col 1: Main Grouped Breakdown (Days, Apps, Hours) with expandable child application lists.
    - Col 2: Secondary Breakdown (Top Applications overall or Daily History with drilldowns).
    - Col 3: Hourly Trends (recent hourly consumption).
    - Col 4: Traffic Type / WAN vs LAN Breakdown.
    Features 100% in-place updates: zero widget thrashing, no scroll resets, and persistent expansion.
    """
    item_clicked = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.expanded_keys = set()
        self.show_all_keys = set()
        self.group_by_mode = "Application"

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        self.card_apps = self.create_card("Daily Breakdown", "📅")
        self.card_days = self.create_card("Top Applications", "📱")
        self.card_hours = self.create_card("Hourly Activity", "🕒")
        self.card_traffic = self.create_card("WAN vs LAN Traffic", "🌐")

        layout.addWidget(self.card_apps)
        layout.addWidget(self.card_days)
        layout.addWidget(self.card_hours)
        layout.addWidget(self.card_traffic)

    def create_card(self, title, icon):
        frame = QFrame()
        frame.setStyleSheet("""
            QFrame {
                background-color: #121927;
                border: 1px solid #1e293b;
                border-radius: 8px;
            }
        """)
        vbox = QVBoxLayout(frame)
        vbox.setContentsMargins(10, 8, 10, 8)
        vbox.setSpacing(6)

        hdr = QLabel(f"{icon}  {title}")
        hdr.setStyleSheet("color: #94a3b8; font-weight: 700; font-size: 12px; border: none; padding-bottom: 4px; border-bottom: 1px solid #1e293b;")
        vbox.addWidget(hdr)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setStyleSheet("background: transparent; border: none;")

        container = QWidget()
        container.setStyleSheet("background: transparent;")
        c_layout = QVBoxLayout(container)
        c_layout.setContentsMargins(0, 4, 0, 0)
        c_layout.setSpacing(8)
        c_layout.addStretch()

        scroll.setWidget(container)
        vbox.addWidget(scroll, stretch=1)

        frame.content_layout = c_layout
        frame.container_widget = container
        frame.hdr_label = hdr
        frame.item_widgets = {}
        return frame

    def _on_item_toggled(self, raw_key, is_expanded):
        if is_expanded:
            self.expanded_keys.add(raw_key)
        else:
            self.expanded_keys.discard(raw_key)

    def _on_item_show_all_toggled(self, raw_key, show_all):
        if show_all:
            self.show_all_keys.add(raw_key)
        else:
            self.show_all_keys.discard(raw_key)

    def populate(self, raw_data, day_items, hour_items, totals, group_by_mode="Day", children_map=None, day_children_map=None, top_apps=None):
        if group_by_mode != self.group_by_mode:
            # Mode changed: clear old widgets cache
            for c in (self.card_apps, self.card_days, self.card_hours):
                if hasattr(c, "item_widgets"):
                    for w in c.item_widgets.values():
                        w.deleteLater()
                    c.item_widgets.clear()
            self.group_by_mode = group_by_mode

        children_map = children_map or {}
        day_children_map = day_children_map or {}
        top_apps = top_apps or []

        # 1. Update Card 1 & Card 2 based on current grouping
        if group_by_mode == "Day":
            self.card_apps.hdr_label.setText("📅 Daily Breakdown (Click Day to Expand Apps)")
            self._populate_list(self.card_apps, raw_data[:15], children_map=children_map, is_drilldown=True)

            self.card_days.hdr_label.setText("📱 Top Applications (Click App to Expand Days)")
            self._populate_list(self.card_days, top_apps[:12], children_map=day_children_map, is_drilldown=True)

        elif group_by_mode == "Application":
            self.card_apps.hdr_label.setText("📱 Applications (Click App to Expand Days)")
            self._populate_list(self.card_apps, raw_data[:15], children_map=children_map, is_drilldown=True)

            self.card_days.hdr_label.setText("📅 Daily Breakdown (Click Day to Expand Apps)")
            self._populate_list(self.card_days, day_items[:12], children_map=day_children_map, is_drilldown=True)

        elif group_by_mode == "Hour":
            self.card_apps.hdr_label.setText("🕒 Hourly Breakdown (Click Hour to Expand Apps)")
            self._populate_list(self.card_apps, raw_data[:15], children_map=children_map, is_drilldown=True)

            self.card_days.hdr_label.setText("📅 Daily Breakdown (Click Day to Expand Apps)")
            self._populate_list(self.card_days, day_items[:12], children_map=day_children_map, is_drilldown=True)

        elif group_by_mode == "Week":
            self.card_apps.hdr_label.setText("📆 Weekly Breakdown (Click Week to Expand Apps)")
            self._populate_list(self.card_apps, raw_data[:15], children_map=children_map, is_drilldown=True)

            self.card_days.hdr_label.setText("📅 Daily Breakdown (Click Day to Expand Apps)")
            self._populate_list(self.card_days, day_items[:12], children_map=day_children_map, is_drilldown=True)

        elif group_by_mode == "Month":
            self.card_apps.hdr_label.setText("🗓️ Monthly Breakdown (Click Month to Expand Apps)")
            self._populate_list(self.card_apps, raw_data[:15], children_map=children_map, is_drilldown=True)

            self.card_days.hdr_label.setText("📅 Daily Breakdown (Click Day to Expand Apps)")
            self._populate_list(self.card_days, day_items[:12], children_map=day_children_map, is_drilldown=True)

        # 3. Hours (flat list with in-place updates)
        self._populate_list(self.card_hours, hour_items[:12], children_map=None, is_drilldown=False)

        # 4. Traffic Summary (in-place updates)
        self._populate_traffic_summary(self.card_traffic, totals)

    def _populate_list(self, card, items, children_map=None, is_drilldown=True):
        """In-place list updater: reuses existing widgets to prevent layout collapse and scroll jumps."""
        layout = card.content_layout
        children_map = children_map or {}

        if not hasattr(card, "item_widgets"):
            card.item_widgets = {}

        if not items:
            for w in card.item_widgets.values():
                w.deleteLater()
            card.item_widgets.clear()
            if not getattr(card, "no_entries_lbl", None):
                card.no_entries_lbl = QLabel("No entries recorded.")
                card.no_entries_lbl.setStyleSheet("color: #64748b; font-size: 11px; padding: 6px;")
                layout.insertWidget(0, card.no_entries_lbl)
            card.no_entries_lbl.setVisible(True)
            return
        elif getattr(card, "no_entries_lbl", None):
            card.no_entries_lbl.setVisible(False)

        max_tot = max((x["total"] for x in items), default=1)
        if max_tot <= 0:
            max_tot = 1

        active_keys = set()

        for idx, item in enumerate(items):
            raw_key = item.get("raw_entity", item.get("entity", ""))
            active_keys.add(raw_key)
            children = children_map.get(raw_key, [])
            is_expanded = (raw_key in self.expanded_keys)
            show_all = (raw_key in self.show_all_keys)

            if is_drilldown:
                if raw_key in card.item_widgets and isinstance(card.item_widgets[raw_key], ExpandableEntityCardItem):
                    # IN-PLACE UPDATE! Zero widget deletion, zero recreation, zero lag!
                    item_w = card.item_widgets[raw_key]
                    if layout.indexOf(item_w) != idx:
                        layout.insertWidget(idx, item_w)
                    item_w.update_data(item, max_tot, children, is_expanded, show_all)
                else:
                    if raw_key in card.item_widgets:
                        card.item_widgets[raw_key].deleteLater()
                    item_w = ExpandableEntityCardItem(
                        item=item,
                        max_parent_tot=max_tot,
                        children=children,
                        is_expanded=is_expanded,
                        show_all=show_all
                    )
                    item_w.toggled.connect(self._on_item_toggled)
                    item_w.show_all_toggled.connect(self._on_item_show_all_toggled)
                    card.item_widgets[raw_key] = item_w
                    layout.insertWidget(idx, item_w)
            else:
                if raw_key in card.item_widgets and not isinstance(card.item_widgets[raw_key], ExpandableEntityCardItem):
                    row_w = card.item_widgets[raw_key]
                    if layout.indexOf(row_w) != idx:
                        layout.insertWidget(idx, row_w)
                    row_w.lbl_name.setText(item.get("entity", ""))
                    row_w.lbl_val.setText(format_bytes(item.get("total", 0)))
                    row_w.bar.set_values(item.get("wan_down", 0) + item.get("lan_down", 0), item.get("wan_up", 0) + item.get("lan_up", 0), max_tot)
                else:
                    if raw_key in card.item_widgets:
                        card.item_widgets[raw_key].deleteLater()
                    row_w = QWidget()
                    row_w.setStyleSheet("QWidget:hover { background-color: #1a2538; border-radius: 4px; }")
                    r_box = QVBoxLayout(row_w)
                    r_box.setContentsMargins(4, 2, 4, 2)
                    r_box.setSpacing(2)

                    top_row = QHBoxLayout()
                    top_row.setSpacing(6)

                    row_w.lbl_name = QLabel(item.get("entity", ""))
                    row_w.lbl_name.setStyleSheet("color: #f1f5f9; font-size: 11px; font-weight: 600; border: none;")
                    top_row.addWidget(row_w.lbl_name, stretch=1)

                    row_w.lbl_val = QLabel(format_bytes(item.get("total", 0)))
                    row_w.lbl_val.setStyleSheet("color: #38bdf8; font-size: 11px; font-weight: 700; border: none;")
                    top_row.addWidget(row_w.lbl_val)
                    r_box.addLayout(top_row)

                    row_w.bar = DualProgressBar()
                    row_w.bar.setFixedHeight(8)
                    row_w.bar.set_values(item.get("wan_down", 0) + item.get("lan_down", 0), item.get("wan_up", 0) + item.get("lan_up", 0), max_tot)
                    r_box.addWidget(row_w.bar)

                    card.item_widgets[raw_key] = row_w
                    layout.insertWidget(idx, row_w)

        # Remove stale widgets that are no longer in items
        stale_keys = [k for k in card.item_widgets if k not in active_keys]
        for k in stale_keys:
            w = card.item_widgets.pop(k)
            w.deleteLater()

    def _populate_traffic_summary(self, card, totals):
        layout = card.content_layout
        if not totals:
            return

        w_tot = totals.get("wan_tot", 0)
        l_tot = totals.get("lan_tot", 0)
        max_tot = max(w_tot, l_tot, 1)

        if not getattr(card, "traffic_inited", False):
            # Build once, then update in-place
            while layout.count() > 1:
                child = layout.takeAt(0)
                if child.widget():
                    child.widget().deleteLater()

            # Internet WAN
            w_widget = QWidget()
            w_box = QVBoxLayout(w_widget)
            w_box.setContentsMargins(4, 4, 4, 4)
            w_box.setSpacing(3)

            w_head = QHBoxLayout()
            w_head.addWidget(QLabel("🌐 Internet (WAN)"))
            card.w_val = QLabel(format_bytes(w_tot))
            card.w_val.setStyleSheet("color: #06b6d4; font-weight: bold;")
            w_head.addWidget(card.w_val)
            w_box.addLayout(w_head)

            card.w_bar = DualProgressBar()
            card.w_bar.setFixedHeight(10)
            card.w_bar.set_values(totals.get("wan_down", 0), totals.get("wan_up", 0), max_tot)
            w_box.addWidget(card.w_bar)

            card.w_sub = QLabel(f"↓ {format_bytes(totals.get('wan_down', 0))}  |  ↑ {format_bytes(totals.get('wan_up', 0))}")
            card.w_sub.setStyleSheet("color: #64748b; font-size: 10px;")
            w_box.addWidget(card.w_sub)
            layout.insertWidget(0, w_widget)

            # Local LAN
            l_widget = QWidget()
            l_box = QVBoxLayout(l_widget)
            l_box.setContentsMargins(4, 4, 4, 4)
            l_box.setSpacing(3)

            l_head = QHBoxLayout()
            l_head.addWidget(QLabel("🏠 Local Network (LAN)"))
            card.l_val = QLabel(format_bytes(l_tot))
            card.l_val.setStyleSheet("color: #a855f7; font-weight: bold;")
            l_head.addWidget(card.l_val)
            l_box.addLayout(l_head)

            card.l_bar = DualProgressBar()
            card.l_bar.setFixedHeight(10)
            card.l_bar.set_values(totals.get("lan_down", 0), totals.get("lan_up", 0), max_tot)
            l_box.addWidget(card.l_bar)

            card.l_sub = QLabel(f"↓ {format_bytes(totals.get('lan_down', 0))}  |  ↑ {format_bytes(totals.get('lan_up', 0))}")
            card.l_sub.setStyleSheet("color: #64748b; font-size: 10px;")
            l_box.addWidget(card.l_sub)
            layout.insertWidget(1, l_widget)

            card.traffic_inited = True
        else:
            # In-place update
            card.w_val.setText(format_bytes(w_tot))
            card.w_bar.set_values(totals.get("wan_down", 0), totals.get("wan_up", 0), max_tot)
            card.w_sub.setText(f"↓ {format_bytes(totals.get('wan_down', 0))}  |  ↑ {format_bytes(totals.get('wan_up', 0))}")

            card.l_val.setText(format_bytes(l_tot))
            card.l_bar.set_values(totals.get("lan_down", 0), totals.get("lan_up", 0), max_tot)
            card.l_sub.setText(f"↓ {format_bytes(totals.get('lan_down', 0))}  |  ↑ {format_bytes(totals.get('lan_up', 0))}")


# ============================================================================
# Multi-Threaded Database Worker
# ============================================================================

class DbQueryWorker(QThread):
    """
    Asynchronous background worker to query SQLite off the main UI thread:
    - Grouped usage records
    - Timeline wave chart data points
    - Top day & hour aggregations for GlassWire cards
    - Top applications breakdown
    - Drilldown hierarchical children (e.g. applications active on each day, or days active for each app)
    """
    data_loaded = Signal(list, dict, list, list, list, dict, dict, list, bool) # raw_data, totals, timeline_pts, day_items, hour_items, children_map, day_children_map, top_apps, daemon_active

    def __init__(self, db_path, time_clause, group_by_mode, timeline_hours=24, parent=None):
        super().__init__(parent)
        self.db_path = db_path
        self.time_clause = time_clause
        self.group_by_mode = group_by_mode
        self.timeline_hours = timeline_hours

    def run(self):
        daemon_active = is_daemon_running_fast()

        if not os.path.exists(self.db_path):
            self.data_loaded.emit([], {}, [], [], [], {}, {}, [], daemon_active)
            return

        # 1. Main Grouped Query
        entity_expr = "app_name"
        order_expr = "grand_total DESC"

        if self.group_by_mode == "Day":
            entity_expr = "strftime('%Y-%m-%d', timestamp)"
            order_expr = "entity DESC"
        elif self.group_by_mode == "Hour":
            entity_expr = "strftime('%Y-%m-%d %H:00', timestamp)"
            order_expr = "entity DESC"
        elif self.group_by_mode == "Week":
            entity_expr = "strftime('%Y-W%W', timestamp)"
            order_expr = "entity DESC"
        elif self.group_by_mode == "Month":
            entity_expr = "strftime('%Y-%m', timestamp)"
            order_expr = "entity DESC"

        main_sql = f"""
            SELECT {entity_expr} AS entity,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS wan_down,
                   SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS wan_up,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                   SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received + bytes_sent ELSE 0 END) AS total_wan,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received + bytes_sent ELSE 0 END) AS total_lan,
                   SUM(bytes_sent + bytes_received) AS grand_total,
                   COUNT(*) AS samples,
                   MAX(timestamp) AS last_seen
            FROM network_usage
            {self.time_clause}
            GROUP BY entity
            ORDER BY {order_expr};
        """

        # 2. Timeline Wave Query (in minute buckets)
        bucket_mins = 10 if self.timeline_hours <= 24 else 30
        timeline_sql = f"""
            SELECT strftime('%Y-%m-%d %H:', timestamp) || printf('%02d', (CAST(strftime('%M', timestamp) AS INTEGER) / {bucket_mins}) * {bucket_mins}) AS t_bucket,
                   SUM(bytes_received) AS rx_bytes,
                   SUM(bytes_sent) AS tx_bytes
            FROM network_usage
            WHERE timestamp >= datetime('now', '-{self.timeline_hours} hours', 'localtime')
            GROUP BY t_bucket
            ORDER BY t_bucket ASC;
        """

        # 3. Days Summary for GlassWire Cards
        days_sql = f"""
            SELECT strftime('%Y-%m-%d', timestamp) AS day_val,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                   SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS l_down,
                   SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS l_up,
                   SUM(bytes_sent + bytes_received) AS grand_total
            FROM network_usage
            {self.time_clause}
            GROUP BY day_val
            ORDER BY day_val DESC
            LIMIT 15;
        """

        # 4. Hours Summary for GlassWire Cards
        hours_sql = """
            SELECT strftime('%Y-%m-%d %H:00', timestamp) AS hr_val,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                   SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS l_down,
                   SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS l_up,
                   SUM(bytes_sent + bytes_received) AS grand_total
            FROM network_usage
            WHERE timestamp >= datetime('now', '-24 hours', 'localtime')
            GROUP BY hr_val
            ORDER BY hr_val DESC
            LIMIT 10;
        """

        # 5. Top Apps Summary for GlassWire Cards
        top_apps_sql = f"""
            SELECT app_name,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                   SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS l_down,
                   SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS l_up,
                   SUM(bytes_sent + bytes_received) AS grand_total
            FROM network_usage
            {self.time_clause}
            GROUP BY app_name
            ORDER BY grand_total DESC
            LIMIT 15;
        """

        # 6. Hierarchical Drilldown Children Query (based on current Group By mode)
        if self.group_by_mode == "Day":
            children_sql = f"""
                SELECT strftime('%Y-%m-%d', timestamp) AS p_key,
                       app_name AS c_name,
                       SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                       SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                       SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                       SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                       SUM(bytes_sent + bytes_received) AS total
                FROM network_usage
                {self.time_clause}
                GROUP BY p_key, c_name
                ORDER BY p_key DESC, total DESC;
            """
        elif self.group_by_mode == "Hour":
            children_sql = f"""
                SELECT strftime('%Y-%m-%d %H:00', timestamp) AS p_key,
                       app_name AS c_name,
                       SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                       SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                       SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                       SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                       SUM(bytes_sent + bytes_received) AS total
                FROM network_usage
                {self.time_clause}
                GROUP BY p_key, c_name
                ORDER BY p_key DESC, total DESC;
            """
        elif self.group_by_mode == "Week":
            children_sql = f"""
                SELECT strftime('%Y-W%W', timestamp) AS p_key,
                       app_name AS c_name,
                       SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                       SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                       SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                       SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                       SUM(bytes_sent + bytes_received) AS total
                FROM network_usage
                {self.time_clause}
                GROUP BY p_key, c_name
                ORDER BY p_key DESC, total DESC;
            """
        elif self.group_by_mode == "Month":
            children_sql = f"""
                SELECT strftime('%Y-%m', timestamp) AS p_key,
                       app_name AS c_name,
                       SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                       SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                       SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                       SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                       SUM(bytes_sent + bytes_received) AS total
                FROM network_usage
                {self.time_clause}
                GROUP BY p_key, c_name
                ORDER BY p_key DESC, total DESC;
            """
        else:  # "Application"
            children_sql = f"""
                SELECT app_name AS p_key,
                       strftime('%Y-%m-%d', timestamp) AS c_name,
                       SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                       SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                       SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                       SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                       SUM(bytes_sent + bytes_received) AS total
                FROM network_usage
                {self.time_clause}
                GROUP BY p_key, c_name
                ORDER BY p_key ASC, total DESC;
            """

        # 7. Day-to-Apps Drilldown Query (always available for Daily Breakdown cards)
        day_apps_sql = f"""
            SELECT strftime('%Y-%m-%d', timestamp) AS p_key,
                   app_name AS c_name,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                   SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                   SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                   SUM(bytes_sent + bytes_received) AS total
            FROM network_usage
            {self.time_clause}
            GROUP BY p_key, c_name
            ORDER BY p_key DESC, total DESC;
        """

        # 8. App-to-Days Query (for when in Day mode so Top Apps card can expand days)
        app_days_sql = f"""
            SELECT app_name AS p_key,
                   strftime('%Y-%m-%d', timestamp) AS c_name,
                   SUM(CASE WHEN is_local = 0 THEN bytes_received ELSE 0 END) AS w_down,
                   SUM(CASE WHEN is_local = 0 THEN bytes_sent ELSE 0 END) AS w_up,
                   SUM(CASE WHEN is_local = 1 THEN bytes_received ELSE 0 END) AS lan_down,
                   SUM(CASE WHEN is_local = 1 THEN bytes_sent ELSE 0 END) AS lan_up,
                   SUM(bytes_sent + bytes_received) AS total
            FROM network_usage
            {self.time_clause}
            GROUP BY p_key, c_name
            ORDER BY p_key ASC, total DESC;
        """

        main_rows = []
        timeline_rows = []
        days_rows = []
        hours_rows = []
        top_apps_rows = []
        children_rows = []
        day_apps_rows = []
        app_days_rows = []

        try:
            conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=2.0)
            cur = conn.cursor()
            cur.execute(main_sql)
            main_rows = cur.fetchall()

            cur.execute(timeline_sql)
            timeline_rows = cur.fetchall()

            cur.execute(days_sql)
            days_rows = cur.fetchall()

            cur.execute(hours_sql)
            hours_rows = cur.fetchall()

            cur.execute(top_apps_sql)
            top_apps_rows = cur.fetchall()

            cur.execute(children_sql)
            children_rows = cur.fetchall()

            if self.group_by_mode != "Day":
                cur.execute(day_apps_sql)
                day_apps_rows = cur.fetchall()
            else:
                cur.execute(app_days_sql)
                app_days_rows = cur.fetchall()

            conn.close()
        except Exception:
            try:
                conn = sqlite3.connect(self.db_path, timeout=2.0)
                cur = conn.cursor()
                cur.execute(main_sql)
                main_rows = cur.fetchall()
                cur.execute(timeline_sql)
                timeline_rows = cur.fetchall()
                cur.execute(days_sql)
                days_rows = cur.fetchall()
                cur.execute(hours_sql)
                hours_rows = cur.fetchall()
                cur.execute(top_apps_sql)
                top_apps_rows = cur.fetchall()
                cur.execute(children_sql)
                children_rows = cur.fetchall()
                if self.group_by_mode != "Day":
                    cur.execute(day_apps_sql)
                    day_apps_rows = cur.fetchall()
                else:
                    cur.execute(app_days_sql)
                    app_days_rows = cur.fetchall()
                conn.close()
            except Exception:
                pass

        # Process Main Rows
        raw_data = []
        tot_w_down = 0
        tot_w_up = 0
        tot_l_down = 0
        tot_l_up = 0
        tot_wan = 0
        tot_lan = 0
        grand_total = 0

        today_str = datetime.now().strftime("%Y-%m-%d")
        yesterday_str = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")

        for r in main_rows:
            entity_name = str(r[0] or "[unknown]")
            w_down = int(r[1] or 0)
            w_up = int(r[2] or 0)
            l_down = int(r[3] or 0)
            l_up = int(r[4] or 0)
            t_wan = int(r[5] or 0)
            t_lan = int(r[6] or 0)
            tot = int(r[7] or 0)
            samples = int(r[8] or 0)
            last_seen = str(r[9] or "-")

            tot_w_down += w_down
            tot_w_up += w_up
            tot_l_down += l_down
            tot_l_up += l_up
            tot_wan += t_wan
            tot_lan += t_lan
            grand_total += tot

            # Friendly day labels
            display_name = entity_name
            if self.group_by_mode == "Day":
                if entity_name == today_str:
                    display_name = f"📅 {entity_name} (Today)"
                elif entity_name == yesterday_str:
                    display_name = f"📅 {entity_name} (Yesterday)"
                else:
                    display_name = f"📅 {entity_name}"
            elif self.group_by_mode == "Hour":
                display_name = f"🕒 {entity_name}"
            elif self.group_by_mode == "Week":
                display_name = f"📆 {entity_name}"
            elif self.group_by_mode == "Month":
                display_name = f"🗓️ {entity_name}"

            raw_data.append({
                "entity": display_name,
                "raw_entity": entity_name,
                "total_wan": t_wan,
                "wan_down": w_down,
                "wan_up": w_up,
                "total_lan": t_lan,
                "lan_down": l_down,
                "lan_up": l_up,
                "total": tot,
                "samples": samples,
                "last_seen": last_seen
            })

        totals = {
            "wan_down": tot_w_down,
            "wan_up": tot_w_up,
            "lan_down": tot_l_down,
            "lan_up": tot_l_up,
            "wan_tot": tot_wan,
            "lan_tot": tot_lan,
            "grand_total": grand_total,
            "count": len(raw_data)
        }

        # Process Timeline Points
        timeline_pts = []
        for tr in timeline_rows:
            try:
                dt = datetime.strptime(tr[0], "%Y-%m-%d %H:%M")
                timeline_pts.append((dt, int(tr[1] or 0), int(tr[2] or 0)))
            except Exception:
                continue

        # Process Days
        day_items = []
        for dr in days_rows:
            d_name = str(dr[0])
            if d_name == today_str:
                d_disp = f"📅 {d_name} (Today)"
            elif d_name == yesterday_str:
                d_disp = f"📅 {d_name} (Yesterday)"
            else:
                d_disp = f"📅 {d_name}"
            day_items.append({
                "entity": d_disp,
                "raw_entity": d_name,
                "wan_down": int(dr[1] or 0),
                "wan_up": int(dr[2] or 0),
                "lan_down": int(dr[3] or 0),
                "lan_up": int(dr[4] or 0),
                "total": int(dr[5] or 0)
            })

        # Process Hours
        hour_items = []
        for hr in hours_rows:
            hour_items.append({
                "entity": str(hr[0]),
                "raw_entity": str(hr[0]),
                "wan_down": int(hr[1] or 0),
                "wan_up": int(hr[2] or 0),
                "lan_down": int(hr[3] or 0),
                "lan_up": int(hr[4] or 0),
                "total": int(hr[5] or 0)
            })

        # Process Top Apps
        top_apps_items = []
        for ar in top_apps_rows:
            top_apps_items.append({
                "entity": f"📦 {str(ar[0])}",
                "raw_entity": str(ar[0]),
                "wan_down": int(ar[1] or 0),
                "wan_up": int(ar[2] or 0),
                "lan_down": int(ar[3] or 0),
                "lan_up": int(ar[4] or 0),
                "total": int(ar[5] or 0)
            })

        # Process Children Map
        children_map = {}
        for cr in children_rows:
            pk = str(cr[0])
            if pk not in children_map:
                children_map[pk] = []
            c_name = str(cr[1])
            if self.group_by_mode == "Application":
                if c_name == today_str:
                    c_disp = f"📅 {c_name} (Today)"
                elif c_name == yesterday_str:
                    c_disp = f"📅 {c_name} (Yesterday)"
                else:
                    c_disp = f"📅 {c_name}"
            else:
                c_disp = c_name

            children_map[pk].append({
                "name": c_disp,
                "raw_name": c_name,
                "wan_down": int(cr[2] or 0),
                "wan_up": int(cr[3] or 0),
                "lan_down": int(cr[4] or 0),
                "lan_up": int(cr[5] or 0),
                "total": int(cr[6] or 0)
            })

        # Process Secondary Children Map (Day-Apps or App-Days)
        day_children_map = {}
        if self.group_by_mode == "Day":
            # In Day mode, secondary card is Top Apps, so we map app -> days
            for ar in app_days_rows:
                pk = str(ar[0])
                if pk not in day_children_map:
                    day_children_map[pk] = []
                c_name = str(ar[1])
                if c_name == today_str:
                    c_disp = f"📅 {c_name} (Today)"
                elif c_name == yesterday_str:
                    c_disp = f"📅 {c_name} (Yesterday)"
                else:
                    c_disp = f"📅 {c_name}"
                day_children_map[pk].append({
                    "name": c_disp,
                    "raw_name": c_name,
                    "wan_down": int(ar[2] or 0),
                    "wan_up": int(ar[3] or 0),
                    "lan_down": int(ar[4] or 0),
                    "lan_up": int(ar[5] or 0),
                    "total": int(ar[6] or 0)
                })
        else:
            # In non-Day modes, secondary card is Daily Breakdown, so map day -> apps
            for dr in day_apps_rows:
                pk = str(dr[0])
                if pk not in day_children_map:
                    day_children_map[pk] = []
                day_children_map[pk].append({
                    "name": str(dr[1]),
                    "raw_name": str(dr[1]),
                    "wan_down": int(dr[2] or 0),
                    "wan_up": int(dr[3] or 0),
                    "lan_down": int(dr[4] or 0),
                    "lan_up": int(dr[5] or 0),
                    "total": int(dr[6] or 0)
                })

        self.data_loaded.emit(
            raw_data,
            totals,
            timeline_pts,
            day_items,
            hour_items,
            children_map,
            day_children_map,
            top_apps_items,
            daemon_active
        )


# ============================================================================
# Database History Maintenance & Retention Dialog
# ============================================================================

class DatabasePurgeDialog(QDialog):
    """
    Advanced Database History Retention & Purge Manager.
    Allows:
    1. Keeping records between a custom Date Range (Start Date to End Date) and deleting all outside.
    2. Keeping records within recent time window (Last 7, 10, 30, 60, 90 days) and deleting older records.
    3. Complete wipe of all records.
    Includes live impact preview and safety confirmation check.
    """
    def __init__(self, db_path, parent=None):
        super().__init__(parent)
        self.db_path = db_path
        self.setWindowTitle("⚠️ Clean & Manage Database History")
        self.setFixedSize(580, 620)
        self.setModal(True)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)

        # 1. Warning Banner
        banner_frame = QFrame()
        banner_frame.setStyleSheet("""
            QFrame {
                background-color: #271418;
                border: 1px solid #7f1d1d;
                border-left: 5px solid #ef4444;
                border-radius: 8px;
            }
        """)
        b_box = QHBoxLayout(banner_frame)
        b_box.setContentsMargins(10, 8, 10, 8)
        b_box.setSpacing(12)

        icon_lbl = QLabel("⚠️")
        icon_lbl.setStyleSheet("font-size: 26px; border: none; background: transparent;")
        b_box.addWidget(icon_lbl)

        b_text_box = QVBoxLayout()
        b_text_box.setSpacing(2)
        b_title = QLabel("Database Retention & Purge Manager")
        b_title.setStyleSheet("color: #fca5a5; font-size: 14px; font-weight: bold; border: none; background: transparent;")
        b_desc = QLabel("Permanently purge records outside a date range, keep recent days/months, or reset all data.")
        b_desc.setStyleSheet("color: #cbd5e1; font-size: 11px; border: none; background: transparent;")
        b_desc.setWordWrap(True)
        b_text_box.addWidget(b_title)
        b_text_box.addWidget(b_desc)
        b_box.addLayout(b_text_box, stretch=1)
        layout.addWidget(banner_frame)

        # 2. Options Group Box
        opts_frame = QFrame()
        opts_frame.setStyleSheet("""
            QFrame {
                background-color: #121927;
                border: 1px solid #1e293b;
                border-radius: 8px;
            }
        """)
        opts_layout = QVBoxLayout(opts_frame)
        opts_layout.setContentsMargins(14, 12, 14, 12)
        opts_layout.setSpacing(12)

        # Option A: Keep Date Range (Delete Outside)
        self.radio_range = QRadioButton("📅 Keep Specific Date Range (Delete Everything Outside)")
        self.radio_range.setChecked(True)
        self.radio_range.setStyleSheet("font-size: 12px; font-weight: bold; color: #f1f5f9;")
        opts_layout.addWidget(self.radio_range)

        range_desc = QLabel("Keeps only records between Start Date and End Date (inclusive).\nAll records recorded before Start Date or after End Date will be permanently deleted.")
        range_desc.setStyleSheet("color: #94a3b8; font-size: 11px; margin-left: 20px;")
        opts_layout.addWidget(range_desc)

        range_row = QHBoxLayout()
        range_row.setContentsMargins(20, 0, 0, 0)
        range_row.setSpacing(8)

        range_row.addWidget(QLabel("Start Date:"))
        self.date_start = QDateEdit()
        self.date_start.setCalendarPopup(True)
        self.date_start.setDate(QtCore.QDate.currentDate().addDays(-7))
        range_row.addWidget(self.date_start)

        range_row.addWidget(QLabel("End Date:"))
        self.date_end = QDateEdit()
        self.date_end.setCalendarPopup(True)
        self.date_end.setDate(QtCore.QDate.currentDate())
        range_row.addWidget(self.date_end)
        range_row.addStretch()
        opts_layout.addLayout(range_row)

        opts_layout.addSpacing(4)
        div1 = QFrame()
        div1.setFrameShape(QFrame.Shape.HLine)
        div1.setStyleSheet("color: #1e293b;")
        opts_layout.addWidget(div1)

        # Option B: Keep Recent Time Window (Purge Older)
        self.radio_recent = QRadioButton("⏱️ Keep Recent History (Purge Older Records)")
        self.radio_recent.setStyleSheet("font-size: 12px; font-weight: bold; color: #f1f5f9;")
        opts_layout.addWidget(self.radio_recent)

        recent_desc = QLabel("Retains records within the selected recent time window and deletes all older history.")
        recent_desc.setStyleSheet("color: #94a3b8; font-size: 11px; margin-left: 20px;")
        opts_layout.addWidget(recent_desc)

        recent_row = QHBoxLayout()
        recent_row.setContentsMargins(20, 0, 0, 0)
        recent_row.setSpacing(8)
        recent_row.addWidget(QLabel("Retention Window:"))
        self.combo_recent = QComboBox()
        self.combo_recent.addItems([
            "Keep Last 7 Days (Purge older than 7 days)",
            "Keep Last 10 Days (Purge older than 10 days)",
            "Keep Last 30 Days / 1 Month (Purge older than 1 month)",
            "Keep Last 60 Days / 2 Months (Purge older than 2 months)",
            "Keep Last 90 Days / 3 Months (Purge older than 3 months)"
        ])
        self.combo_recent.setCurrentIndex(1)
        self.combo_recent.setMinimumWidth(280)
        recent_row.addWidget(self.combo_recent)
        recent_row.addStretch()
        opts_layout.addLayout(recent_row)

        opts_layout.addSpacing(4)
        div2 = QFrame()
        div2.setFrameShape(QFrame.Shape.HLine)
        div2.setStyleSheet("color: #1e293b;")
        opts_layout.addWidget(div2)

        # Option C: Complete Wipe
        self.radio_all = QRadioButton("🚨 Complete Database Reset (Delete ALL Records)")
        self.radio_all.setStyleSheet("font-size: 12px; font-weight: bold; color: #fca5a5;")
        opts_layout.addWidget(self.radio_all)

        all_desc = QLabel("Permanently wipes every single recorded network usage sample from the beginning of time.")
        all_desc.setStyleSheet("color: #94a3b8; font-size: 11px; margin-left: 20px;")
        opts_layout.addWidget(all_desc)

        layout.addWidget(opts_frame)

        # 3. Live Projected Impact Card
        impact_frame = QFrame()
        impact_frame.setStyleSheet("""
            QFrame {
                background-color: #151d2b;
                border: 1px solid #233045;
                border-left: 4px solid #38bdf8;
                border-radius: 8px;
            }
        """)
        imp_box = QVBoxLayout(impact_frame)
        imp_box.setContentsMargins(12, 8, 12, 8)
        imp_box.setSpacing(6)

        imp_title = QLabel("📊 Projected Database Impact:")
        imp_title.setStyleSheet("color: #94a3b8; font-size: 11px; font-weight: 700; border: none;")
        imp_box.addWidget(imp_title)

        stats_row = QHBoxLayout()
        stats_row.setSpacing(16)

        # Total
        c_tot = QVBoxLayout()
        c_tot.setSpacing(1)
        c_tot.addWidget(QLabel("📦 Total In DB:"))
        self.lbl_impact_total = QLabel("0")
        self.lbl_impact_total.setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: bold;")
        c_tot.addWidget(self.lbl_impact_total)
        stats_row.addLayout(c_tot)

        # To Delete
        c_del = QVBoxLayout()
        c_del.setSpacing(1)
        c_del.addWidget(QLabel("🗑️ To Delete:"))
        self.lbl_impact_del = QLabel("0")
        self.lbl_impact_del.setStyleSheet("color: #f43f5e; font-size: 15px; font-weight: 800;")
        c_del.addWidget(self.lbl_impact_del)
        stats_row.addLayout(c_del)

        # To Keep
        c_keep = QVBoxLayout()
        c_keep.setSpacing(1)
        c_keep.addWidget(QLabel("💾 To Keep:"))
        self.lbl_impact_keep = QLabel("0")
        self.lbl_impact_keep.setStyleSheet("color: #10b981; font-size: 15px; font-weight: 800;")
        c_keep.addWidget(self.lbl_impact_keep)
        stats_row.addLayout(c_keep)

        stats_row.addStretch()
        imp_box.addLayout(stats_row)
        layout.addWidget(impact_frame)

        # 4. Mandatory Confirmation Checkbox
        self.confirm_chk = QCheckBox("I understand this action is permanent and cannot be undone.")
        self.confirm_chk.setStyleSheet("color: #fca5a5; font-size: 12px; font-weight: 600;")
        layout.addWidget(self.confirm_chk)

        # 5. Buttons
        btn_row = QHBoxLayout()
        btn_row.addStretch()

        self.btn_cancel = QPushButton("Cancel")
        self.btn_cancel.clicked.connect(self.reject)
        btn_row.addWidget(self.btn_cancel)

        self.btn_execute = QPushButton("🗑️ Permanently Delete Records")
        self.btn_execute.setObjectName("DangerBtn")
        self.btn_execute.setStyleSheet("""
            QPushButton#DangerBtn {
                background-color: #7f1d1d;
                color: #ffffff;
                border: 1px solid #b91c1c;
                font-weight: bold;
                padding: 6px 14px;
            }
            QPushButton#DangerBtn:hover {
                background-color: #991b1b;
            }
            QPushButton#DangerBtn:disabled {
                background-color: #33151b;
                color: #64748b;
                border-color: #451a22;
            }
        """)
        self.btn_execute.setEnabled(False)
        self.btn_execute.clicked.connect(self.execute_purge)
        btn_row.addWidget(self.btn_execute)

        layout.addLayout(btn_row)

        # Connect event handlers
        self.radio_range.toggled.connect(self.on_mode_changed)
        self.radio_recent.toggled.connect(self.on_mode_changed)
        self.radio_all.toggled.connect(self.on_mode_changed)

        self.date_start.dateChanged.connect(self.update_impact_preview)
        self.date_end.dateChanged.connect(self.update_impact_preview)
        self.combo_recent.currentIndexChanged.connect(self.update_impact_preview)
        self.confirm_chk.stateChanged.connect(self.on_confirm_toggled)

        self.on_mode_changed()

    def on_mode_changed(self):
        is_range = self.radio_range.isChecked()
        is_recent = self.radio_recent.isChecked()

        self.date_start.setEnabled(is_range)
        self.date_end.setEnabled(is_range)
        self.combo_recent.setEnabled(is_recent)

        self.update_impact_preview()

    def on_confirm_toggled(self, state):
        confirmed = (state == Qt.CheckState.Checked.value)
        self.btn_execute.setEnabled(confirmed)

    def update_impact_preview(self):
        if not os.path.exists(self.db_path):
            self.lbl_impact_total.setText("0")
            self.lbl_impact_del.setText("0")
            self.lbl_impact_keep.setText("0")
            return

        total_cnt = 0
        del_cnt = 0

        try:
            conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=2.0)
            cur = conn.cursor()

            cur.execute("SELECT COUNT(*) FROM network_usage;")
            r = cur.fetchone()
            total_cnt = int(r[0] or 0) if r else 0

            if self.radio_range.isChecked():
                s_str = self.date_start.date().toString("yyyy-MM-dd")
                e_str = self.date_end.date().toString("yyyy-MM-dd")
                cur.execute(
                    "SELECT COUNT(*) FROM network_usage WHERE date(timestamp) < date(?) OR date(timestamp) > date(?);",
                    (s_str, e_str)
                )
                r = cur.fetchone()
                del_cnt = int(r[0] or 0) if r else 0

            elif self.radio_recent.isChecked():
                idx = self.combo_recent.currentIndex()
                days_map = [7, 10, 30, 60, 90]
                days = days_map[idx] if idx < len(days_map) else 10
                cur.execute(
                    f"SELECT COUNT(*) FROM network_usage WHERE timestamp < datetime('now', '-{days} days', 'localtime');"
                )
                r = cur.fetchone()
                del_cnt = int(r[0] or 0) if r else 0

            elif self.radio_all.isChecked():
                del_cnt = total_cnt

            conn.close()
        except Exception:
            pass

        keep_cnt = max(0, total_cnt - del_cnt)
        self.lbl_impact_total.setText(f"{total_cnt:,}")
        self.lbl_impact_del.setText(f"{del_cnt:,}")
        self.lbl_impact_keep.setText(f"{keep_cnt:,}")

    def execute_purge(self):
        if not os.access(self.db_path, os.W_OK):
            QMessageBox.warning(
                self, "Permission Denied",
                f"The database at:\n{self.db_path}\nis not writable by your current user.\n\n"
                f"Please run the maintenance command using sudo in terminal:\n\n"
                f"sudo netmon --clear"
            )
            return

        del_sql = ""
        params = ()
        desc = ""

        if self.radio_range.isChecked():
            s_str = self.date_start.date().toString("yyyy-MM-dd")
            e_str = self.date_end.date().toString("yyyy-MM-dd")
            del_sql = "DELETE FROM network_usage WHERE date(timestamp) < date(?) OR date(timestamp) > date(?);"
            params = (s_str, e_str)
            desc = f"Retained records between {s_str} and {e_str}."

        elif self.radio_recent.isChecked():
            idx = self.combo_recent.currentIndex()
            days_map = [7, 10, 30, 60, 90]
            days = days_map[idx] if idx < len(days_map) else 10
            del_sql = f"DELETE FROM network_usage WHERE timestamp < datetime('now', '-{days} days', 'localtime');"
            desc = f"Retained last {days} days of history."

        elif self.radio_all.isChecked():
            del_sql = "DELETE FROM network_usage;"
            desc = "Full database reset."

        try:
            conn = sqlite3.connect(self.db_path, timeout=5.0)
            cur = conn.cursor()
            if params:
                cur.execute(del_sql, params)
            else:
                cur.execute(del_sql)
            cur.execute("VACUUM;")
            conn.commit()
            conn.close()

            QMessageBox.information(
                self, "Database Purge Complete",
                f"Successfully completed maintenance:\n{desc}\n\nDatabase has been compacted."
            )
            self.accept()
        except Exception as e:
            QMessageBox.critical(self, "Error Purging Records", f"Failed to modify database:\n{str(e)}")


# ============================================================================
# Main Application Window
# ============================================================================

class NetMonitorApp(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("NetMonitor — Real-Time Linux Telemetry & Packet Monitor")
        self.resize(1220, 820)
        self.setMinimumSize(980, 620)

        icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "netmon.png")
        if os.path.exists(icon_path):
            self.setWindowIcon(QIcon(icon_path))

        self.db_path = get_db_path()
        self.raw_data = []
        self.filtered_data = []
        self.totals = {}
        self.timeline_hours = 24
        self.group_by_mode = "Application"
        self.worker = None

        # Speed measurement state
        self.prev_rx_bytes = None
        self.prev_tx_bytes = None
        self.prev_check_time = None
        self.curr_rx_speed = 0
        self.curr_tx_speed = 0

        self._syncing_time_filter = False

        self.init_ui()
        self.apply_theme()

        # Search debounce timer (150ms)
        self.search_debounce = QTimer(self)
        self.search_debounce.setSingleShot(True)
        self.search_debounce.setInterval(150)
        self.search_debounce.timeout.connect(self.apply_client_filters)

        # Time filter debounce timer (300ms)
        self.time_filter_debounce = QTimer(self)
        self.time_filter_debounce.setSingleShot(True)
        self.time_filter_debounce.setInterval(300)
        self.time_filter_debounce.timeout.connect(self.trigger_refresh)

        # Auto-refresh timer
        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.trigger_refresh)
        self.refresh_timer.start(3000)

        self.trigger_refresh()

    def init_ui(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        main_layout.setContentsMargins(14, 10, 14, 10)
        main_layout.setSpacing(8)

        # ---------------------------------------------------------
        # 1. Top Navigation & Header Bar
        # ---------------------------------------------------------
        top_bar = QHBoxLayout()
        top_bar.setSpacing(10)

        # Brand / Logo
        brand_box = QHBoxLayout()
        brand_icon = QLabel("🌐")
        brand_icon.setStyleSheet("font-size: 22px;")
        brand_box.addWidget(brand_icon)

        title_vbox = QVBoxLayout()
        title_vbox.setSpacing(0)
        lbl_app_name = QLabel("NetMonitor")
        lbl_app_name.setStyleSheet("font-size: 16px; font-weight: 800; color: #f8fafc;")
        lbl_tagline = QLabel("Real-Time Telemetry & Quota Monitor")
        lbl_tagline.setStyleSheet("font-size: 10px; color: #64748b; font-weight: 500;")
        title_vbox.addWidget(lbl_app_name)
        title_vbox.addWidget(lbl_tagline)
        brand_box.addLayout(title_vbox)
        top_bar.addLayout(brand_box)

        top_bar.addSpacing(15)

        # View Mode Switcher (GlassWire Cards vs Detailed Table)
        view_switch_box = QHBoxLayout()
        view_switch_box.setSpacing(2)

        self.btn_view_cards = QPushButton("📊 Cards View")
        self.btn_view_cards.setCheckable(True)
        self.btn_view_cards.setChecked(True)

        self.btn_view_table = QPushButton("📋 Data Table")
        self.btn_view_table.setCheckable(True)

        self.view_btn_group = QButtonGroup(self)
        self.view_btn_group.addButton(self.btn_view_cards, 0)
        self.view_btn_group.addButton(self.btn_view_table, 1)
        self.view_btn_group.idClicked.connect(self.on_view_mode_changed)

        view_switch_box.addWidget(self.btn_view_cards)
        view_switch_box.addWidget(self.btn_view_table)
        top_bar.addLayout(view_switch_box)

        top_bar.addStretch()

        # Daemon Status Badge
        self.status_badge = QLabel(" Checking... ")
        self.status_badge.setObjectName("StatusBadge")
        top_bar.addWidget(self.status_badge)

        # Auto-Refresh Toggle
        self.auto_refresh_chk = QCheckBox("Auto-Refresh")
        self.auto_refresh_chk.setChecked(True)
        self.auto_refresh_chk.stateChanged.connect(self.on_auto_refresh_toggled)
        top_bar.addWidget(self.auto_refresh_chk)

        # Interval
        self.interval_combo = QComboBox()
        self.interval_combo.addItems(["1s", "2s", "3s", "5s", "10s", "30s"])
        self.interval_combo.setCurrentText("3s")
        self.interval_combo.currentIndexChanged.connect(self.on_interval_changed)
        self.interval_combo.setFixedWidth(65)
        top_bar.addWidget(self.interval_combo)

        # Refresh Button
        self.refresh_btn = QPushButton("🔄 Refresh")
        self.refresh_btn.setShortcut("Ctrl+R")
        self.refresh_btn.clicked.connect(self.trigger_refresh)
        top_bar.addWidget(self.refresh_btn)

        # Clear / Purge DB Button
        self.clear_btn = QPushButton("🗑️ Clean / Purge DB")
        self.clear_btn.setObjectName("DangerBtn")
        self.clear_btn.setToolTip("Manage data retention, purge date ranges, or clear historical usage records")
        self.clear_btn.clicked.connect(self.on_clear_db_clicked)
        top_bar.addWidget(self.clear_btn)

        main_layout.addLayout(top_bar)

        # ---------------------------------------------------------
        # 2. Controls Toolbar (Group By, Time Filter, Search, Export)
        # ---------------------------------------------------------
        toolbar_frame = QFrame()
        toolbar_frame.setObjectName("ToolbarFrame")
        tb_main_layout = QVBoxLayout(toolbar_frame)
        tb_main_layout.setContentsMargins(10, 7, 10, 7)
        tb_main_layout.setSpacing(6)

        # ------------------ Row 1: Primary Controls ------------------
        row1 = QHBoxLayout()
        row1.setSpacing(10)

        # Group By Selector (Day, Hour, Week, Month, App)
        row1.addWidget(QLabel("🗂️ Group By:"))
        self.group_by_combo = QComboBox()
        self.group_by_combo.addItems([
            "Application",
            "Day (Daily Usage)",
            "Hour (Hourly Usage)",
            "Week (Weekly Usage)",
            "Month (Monthly Usage)"
        ])
        self.group_by_combo.currentIndexChanged.connect(self.on_group_by_changed)
        row1.addWidget(self.group_by_combo)

        row1.addSpacing(6)

        # Minimum Size Filter
        row1.addWidget(QLabel("📦 Min Size:"))
        self.min_size_combo = QComboBox()
        self.min_size_combo.addItems([
            "All Sizes", "≥ 100 KB", "≥ 1 MB", "≥ 10 MB", "≥ 100 MB", "≥ 500 MB", "≥ 1 GB"
        ])
        self.min_size_combo.currentIndexChanged.connect(self.apply_client_filters)
        row1.addWidget(self.min_size_combo)

        row1.addSpacing(6)

        # Search Bar
        row1.addWidget(QLabel("🔍 Search:"))
        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText("Filter name...")
        self.search_input.setClearButtonEnabled(True)
        self.search_input.textChanged.connect(lambda: self.search_debounce.start())
        row1.addWidget(self.search_input, stretch=1)

        # Export CSV Button
        self.export_btn = QPushButton("💾 CSV")
        self.export_btn.setToolTip("Export currently filtered usage data to CSV")
        self.export_btn.clicked.connect(self.export_to_csv)
        row1.addWidget(self.export_btn)

        tb_main_layout.addLayout(row1)

        # ------------------ Row 2: Comprehensive Time Filter Bar ------------------
        row2 = QHBoxLayout()
        row2.setSpacing(8)

        lbl_time = QLabel("🕒 Time Filter:")
        lbl_time.setStyleSheet("font-weight: 700; color: #94a3b8;")
        row2.addWidget(lbl_time)

        # Presets dropdown
        self.time_combo = QComboBox()
        self.time_combo.addItems([
            "All Time",
            "Last 5 Minutes",
            "Last 15 Minutes",
            "Last 30 Minutes",
            "Last 1 Hour",
            "Last 6 Hours",
            "Last 24 Hours (Today)",
            "Last 7 Days (Week)",
            "Last 30 Days (Month)",
            "Custom Duration",
            "Specific Date..."
        ])
        self.time_combo.setCurrentText("All Time")
        self.time_combo.currentIndexChanged.connect(self.on_time_preset_changed)
        row2.addWidget(self.time_combo)

        # Container frame for the 5 duration boxes
        time_boxes_frame = QFrame()
        time_boxes_frame.setStyleSheet("""
            QFrame {
                background-color: #0b111e;
                border: 1px solid #1e293b;
                border-radius: 6px;
            }
        """)
        tf_layout = QHBoxLayout(time_boxes_frame)
        tf_layout.setContentsMargins(6, 2, 6, 2)
        tf_layout.setSpacing(6)

        def create_spin_field(name, max_val, tooltip):
            lbl = QLabel(name)
            lbl.setStyleSheet("color: #cbd5e1; font-size: 11px; font-weight: 600; border: none; background: transparent;")
            spin = QSpinBox()
            spin.setRange(0, max_val)
            spin.setValue(0)
            spin.setToolTip(tooltip)
            spin.setAlignment(Qt.AlignmentFlag.AlignCenter)
            spin.setFixedWidth(56)
            spin.setStyleSheet("""
                QSpinBox {
                    background-color: #161e2e;
                    color: #38bdf8;
                    border: 1px solid #28354b;
                    border-radius: 4px;
                    padding: 2px 4px;
                    font-size: 12px;
                    font-weight: bold;
                }
                QSpinBox:hover, QSpinBox:focus {
                    border-color: #38bdf8;
                    background-color: #1e293b;
                }
            """)
            spin.valueChanged.connect(self.on_spin_duration_changed)
            spin.lineEdit().returnPressed.connect(self.trigger_refresh)
            tf_layout.addWidget(lbl)
            tf_layout.addWidget(spin)
            return spin

        self.spin_months = create_spin_field("Months:", 120, "Filter by past months (each = 30 days)")
        self.spin_weeks = create_spin_field("Weeks:", 520, "Filter by past weeks (each = 7 days)")
        self.spin_days = create_spin_field("Days:", 3650, "Filter by past days (each = 24 hours)")
        self.spin_hours = create_spin_field("Hours:", 87600, "Filter by past hours (each = 60 minutes)")
        self.spin_minutes = create_spin_field("Minutes:", 5256000, "Filter by past minutes")

        row2.addWidget(time_boxes_frame)

        # Reset button
        self.btn_reset_time = QPushButton("⟲ All Time")
        self.btn_reset_time.setToolTip("Reset time filter to 0 (All Time)")
        self.btn_reset_time.clicked.connect(self.on_reset_time_clicked)
        row2.addWidget(self.btn_reset_time)

        # Specific Date Picker
        self.custom_date_edit = QDateEdit()
        self.custom_date_edit.setCalendarPopup(True)
        self.custom_date_edit.setDate(QtCore.QDate.currentDate())
        self.custom_date_edit.setVisible(False)
        self.custom_date_edit.dateChanged.connect(self.trigger_refresh)
        row2.addWidget(self.custom_date_edit)

        # Duration Summary Badge
        self.lbl_time_summary = QLabel("🌐 All Time")
        self.lbl_time_summary.setStyleSheet("""
            color: #94a3b8;
            background-color: #0f172a;
            border: 1px solid #1e293b;
            border-radius: 4px;
            padding: 3px 8px;
            font-size: 11px;
            font-weight: 700;
        """)
        row2.addWidget(self.lbl_time_summary)

        row2.addStretch()

        tb_main_layout.addLayout(row2)
        main_layout.addWidget(toolbar_frame)

        # ---------------------------------------------------------
        # 3. Middle Section: Stacked Views (Cards View vs Table View)
        # ---------------------------------------------------------
        self.stacked_views = QStackedWidget()

        # View 0: GlassWire Multi-Column Cards View
        self.cards_view = GlassWireCardsWidget()
        self.stacked_views.addWidget(self.cards_view)

        # View 1: Detailed Table View with All Columns
        self.table = QTableWidget()
        self.table.setColumnCount(10)
        self.table.setHorizontalHeaderLabels([
            "Application",
            "Total WAN",
            "WAN Download (RX)",
            "WAN Upload (TX)",
            "Total LAN",
            "LAN Download (RX)",
            "LAN Upload (TX)",
            "Total Usage",
            "Samples",
            "Last Seen"
        ])
        self.table.setAlternatingRowColors(True)
        self.table.setSortingEnabled(True)
        self.table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.verticalHeader().setVisible(False)
        self.table.setShowGrid(False)

        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        for col in range(1, 10):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.ResizeToContents)

        self.table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.table.customContextMenuRequested.connect(self.on_table_context_menu)

        self.stacked_views.addWidget(self.table)
        main_layout.addWidget(self.stacked_views, stretch=1)

        # ---------------------------------------------------------
        # 4. GlassWire Bottom Telemetry & Timeline Panel
        # ---------------------------------------------------------
        bottom_panel = QFrame()
        bottom_panel.setObjectName("BottomPanel")
        b_layout = QVBoxLayout(bottom_panel)
        b_layout.setContentsMargins(12, 8, 12, 8)
        b_layout.setSpacing(6)

        # Row A: Metrics, Speedometer, and Dual WAN/LAN Progress Bars
        row_a = QHBoxLayout()
        row_a.setSpacing(14)

        # Left Download Metric
        left_down_box = QVBoxLayout()
        left_down_box.setSpacing(1)
        self.lbl_down_val = QLabel("0 B  ↓")
        self.lbl_down_val.setStyleSheet("color: #fbbf24; font-size: 16px; font-weight: 800;")
        self.lbl_down_speed = QLabel("0 B/s")
        self.lbl_down_speed.setStyleSheet("color: #94a3b8; font-size: 11px; font-weight: 600;")
        left_down_box.addWidget(self.lbl_down_val)
        left_down_box.addWidget(self.lbl_down_speed)
        row_a.addLayout(left_down_box)

        # Center Speedometer Arc
        self.arc_gauge = SpeedometerArc()
        row_a.addWidget(self.arc_gauge)

        # Right Upload Metric
        right_up_box = QVBoxLayout()
        right_up_box.setSpacing(1)
        self.lbl_up_val = QLabel("0 B  ↑")
        self.lbl_up_val.setStyleSheet("color: #ec4899; font-size: 16px; font-weight: 800;")
        self.lbl_up_speed = QLabel("0 B/s")
        self.lbl_up_speed.setStyleSheet("color: #94a3b8; font-size: 11px; font-weight: 600;")
        right_up_box.addWidget(self.lbl_up_val)
        right_up_box.addWidget(self.lbl_up_speed)
        row_a.addLayout(right_up_box)

        # Divider line
        div = QFrame()
        div.setFrameShape(QFrame.Shape.VLine)
        div.setStyleSheet("color: #233045;")
        row_a.addWidget(div)

        # Dual WAN and LAN Stacked Bars
        wan_lan_box = QVBoxLayout()
        wan_lan_box.setSpacing(4)

        # WAN Row
        wan_row = QHBoxLayout()
        wan_row.setSpacing(8)
        lbl_wan_title = QLabel("WAN")
        lbl_wan_title.setFixedWidth(36)
        lbl_wan_title.setStyleSheet("color: #94a3b8; font-weight: 800; font-size: 11px;")
        self.lbl_wan_num = QLabel("0 B")
        self.lbl_wan_num.setFixedWidth(75)
        self.lbl_wan_num.setStyleSheet("color: #f8fafc; font-weight: 700; font-size: 11px;")
        self.bar_wan = DualProgressBar()
        wan_row.addWidget(lbl_wan_title)
        wan_row.addWidget(self.lbl_wan_num)
        wan_row.addWidget(self.bar_wan, stretch=1)
        wan_lan_box.addLayout(wan_row)

        # LAN Row
        lan_row = QHBoxLayout()
        lan_row.setSpacing(8)
        lbl_lan_title = QLabel("LAN")
        lbl_lan_title.setFixedWidth(36)
        lbl_lan_title.setStyleSheet("color: #94a3b8; font-weight: 800; font-size: 11px;")
        self.lbl_lan_num = QLabel("0 B")
        self.lbl_lan_num.setFixedWidth(75)
        self.lbl_lan_num.setStyleSheet("color: #f8fafc; font-weight: 700; font-size: 11px;")
        self.bar_lan = DualProgressBar()
        lan_row.addWidget(lbl_lan_title)
        lan_row.addWidget(self.lbl_lan_num)
        lan_row.addWidget(self.bar_lan, stretch=1)
        wan_lan_box.addLayout(lan_row)

        row_a.addLayout(wan_lan_box, stretch=1)
        b_layout.addLayout(row_a)

        # Row B: Interactive Timeline Header (with 12h, 24h, 48h, 72h buttons)
        timeline_header = QHBoxLayout()
        timeline_header.setSpacing(6)

        t_title = QLabel("📈 Activity Timeline")
        t_title.setStyleSheet("color: #94a3b8; font-size: 11px; font-weight: 700;")
        timeline_header.addWidget(t_title)

        # Legend
        lbl_leg_rx = QLabel("■ Download")
        lbl_leg_rx.setStyleSheet("color: #fbbf24; font-size: 10px; font-weight: 600;")
        lbl_leg_tx = QLabel("■ Upload")
        lbl_leg_tx.setStyleSheet("color: #ec4899; font-size: 10px; font-weight: 600;")
        timeline_header.addWidget(lbl_leg_rx)
        timeline_header.addWidget(lbl_leg_tx)

        timeline_header.addStretch()

        # Window Range Buttons: 12h, 24h, 48h, 72h
        self.btn_12h = QPushButton("12 Hours")
        self.btn_24h = QPushButton("24 Hours")
        self.btn_48h = QPushButton("48 Hours")
        self.btn_72h = QPushButton("72 Hours")

        for b in [self.btn_12h, self.btn_24h, self.btn_48h, self.btn_72h]:
            b.setCheckable(True)
            b.setStyleSheet("""
                QPushButton {
                    padding: 2px 7px;
                    font-size: 10px;
                    border-radius: 4px;
                    background-color: #1e293b;
                    color: #94a3b8;
                    border: 1px solid #334155;
                }
                QPushButton:checked {
                    background-color: #0284c7;
                    color: #ffffff;
                    border-color: #38bdf8;
                    font-weight: bold;
                }
            """)

        self.btn_24h.setChecked(True)

        self.timeline_range_group = QButtonGroup(self)
        self.timeline_range_group.addButton(self.btn_12h, 12)
        self.timeline_range_group.addButton(self.btn_24h, 24)
        self.timeline_range_group.addButton(self.btn_48h, 48)
        self.timeline_range_group.addButton(self.btn_72h, 72)
        self.timeline_range_group.idClicked.connect(self.on_timeline_range_changed)

        timeline_header.addWidget(self.btn_12h)
        timeline_header.addWidget(self.btn_24h)
        timeline_header.addWidget(self.btn_48h)
        timeline_header.addWidget(self.btn_72h)

        b_layout.addLayout(timeline_header)

        # Row C: Timeline Wave Chart
        self.timeline_chart = TimelineWaveChart()
        b_layout.addWidget(self.timeline_chart)

        main_layout.addWidget(bottom_panel)

    def apply_theme(self):
        self.setStyleSheet("""
            QMainWindow { background-color: #0b0f17; }
            QWidget { color: #f1f5f9; font-family: 'Segoe UI', 'Ubuntu', 'Inter', sans-serif; }
            QFrame#ToolbarFrame, QFrame#BottomPanel {
                background-color: #121927;
                border: 1px solid #1e293b;
                border-radius: 8px;
            }
            QLabel#StatusBadge {
                border-radius: 6px;
                padding: 4px 8px;
                font-size: 11px;
                font-weight: 600;
            }
            QPushButton {
                background-color: #1e293b;
                color: #f8fafc;
                border: 1px solid #334155;
                border-radius: 6px;
                padding: 5px 11px;
                font-weight: 600;
                font-size: 12px;
            }
            QPushButton:hover { background-color: #2e3d55; border-color: #475569; }
            QPushButton:checked {
                background-color: #1d4ed8;
                border-color: #3b82f6;
                color: #ffffff;
            }
            QPushButton#DangerBtn {
                background-color: #3f151e;
                color: #fca5a5;
                border: 1px solid #7f1d1d;
            }
            QPushButton#DangerBtn:hover { background-color: #5b1d2a; border-color: #991b1b; }
            QComboBox, QLineEdit, QDateEdit, QSpinBox {
                background-color: #161e2e;
                color: #f8fafc;
                border: 1px solid #28354b;
                border-radius: 6px;
                padding: 4px 6px;
                font-size: 12px;
            }
            QComboBox:hover, QLineEdit:hover, QDateEdit:hover, QSpinBox:hover, QSpinBox:focus { border-color: #38bdf8; }
            QSpinBox::up-button, QSpinBox::down-button {
                width: 13px;
                background-color: #1e293b;
                border: none;
                border-radius: 2px;
                margin: 1px;
            }
            QSpinBox::up-button:hover, QSpinBox::down-button:hover { background-color: #38bdf8; }
            QCheckBox { color: #94a3b8; font-size: 12px; }
            QCheckBox::indicator {
                width: 15px; height: 15px; border-radius: 4px;
                border: 1px solid #334155; background-color: #161e2e;
            }
            QCheckBox::indicator:checked { background-color: #06b6d4; border-color: #06b6d4; }
            QTableWidget {
                background-color: #101623;
                border: 1px solid #1e293b;
                border-radius: 8px;
                gridline-color: transparent;
                selection-background-color: #1e3a5f;
                selection-color: #ffffff;
            }
            QTableWidget::item { padding: 5px 8px; border-bottom: 1px solid #151e2c; }
            QTableWidget::item:alternate { background-color: #121927; }
            QTableWidget::item:selected { background-color: #1d3557; color: #f8fafc; }
            QHeaderView::section {
                background-color: #161e2e;
                color: #94a3b8;
                padding: 7px 9px;
                font-weight: 700;
                font-size: 11px;
                border: none;
                border-bottom: 2px solid #232f45;
                text-transform: uppercase;
                letter-spacing: 0.5px;
            }
            QHeaderView::section:hover { color: #38bdf8; background-color: #1a2538; }
            QScrollBar:vertical {
                background: #0f172a; width: 10px; margin: 0px; border-radius: 5px;
            }
            QScrollBar::handle:vertical { background: #28354b; min-height: 20px; border-radius: 5px; }
            QScrollBar::handle:vertical:hover { background: #38bdf8; }
        """)

    def on_view_mode_changed(self, view_id):
        self.stacked_views.setCurrentIndex(view_id)

    def on_timeline_range_changed(self, hours):
        self.timeline_hours = hours
        self.trigger_refresh()

    def on_group_by_changed(self):
        txt = self.group_by_combo.currentText()
        if "Day" in txt:
            self.group_by_mode = "Day"
            self.table.horizontalHeaderItem(0).setText("Date / Day")
        elif "Hour" in txt:
            self.group_by_mode = "Hour"
            self.table.horizontalHeaderItem(0).setText("Hour Period")
        elif "Week" in txt:
            self.group_by_mode = "Week"
            self.table.horizontalHeaderItem(0).setText("Week")
        elif "Month" in txt:
            self.group_by_mode = "Month"
            self.table.horizontalHeaderItem(0).setText("Month")
        else:
            self.group_by_mode = "Application"
            self.table.horizontalHeaderItem(0).setText("Application")

        self.trigger_refresh()

    def on_auto_refresh_toggled(self, state):
        if state == Qt.CheckState.Checked.value:
            self.refresh_timer.start()
        else:
            self.refresh_timer.stop()

    def on_interval_changed(self):
        text = self.interval_combo.currentText()
        sec = int(text.replace("s", ""))
        self.refresh_timer.setInterval(sec * 1000)

    def get_filter_duration_seconds(self):
        m = self.spin_months.value()
        w = self.spin_weeks.value()
        d = self.spin_days.value()
        h = self.spin_hours.value()
        mi = self.spin_minutes.value()

        total_minutes = (m * 30 * 1440) + (w * 7 * 1440) + (d * 1440) + (h * 60) + mi
        return total_minutes * 60

    def on_time_preset_changed(self):
        if self._syncing_time_filter:
            return
        preset = self.time_combo.currentText()
        self._syncing_time_filter = True
        try:
            if preset == "Custom Duration":
                self.custom_date_edit.setVisible(False)
                return
            elif preset == "Specific Date...":
                self.custom_date_edit.setVisible(True)
                self.spin_months.setValue(0)
                self.spin_weeks.setValue(0)
                self.spin_days.setValue(0)
                self.spin_hours.setValue(0)
                self.spin_minutes.setValue(0)
            else:
                self.custom_date_edit.setVisible(False)
                m, w, d, h, mi = 0, 0, 0, 0, 0
                if preset == "Last 5 Minutes":
                    mi = 5
                elif preset == "Last 15 Minutes":
                    mi = 15
                elif preset == "Last 30 Minutes":
                    mi = 30
                elif preset == "Last 1 Hour":
                    h = 1
                elif preset == "Last 6 Hours":
                    h = 6
                elif preset == "Last 24 Hours (Today)":
                    d = 1
                elif preset == "Last 7 Days (Week)":
                    w = 1
                elif preset == "Last 30 Days (Month)":
                    m = 1
                elif preset == "All Time":
                    pass

                self.spin_months.setValue(m)
                self.spin_weeks.setValue(w)
                self.spin_days.setValue(d)
                self.spin_hours.setValue(h)
                self.spin_minutes.setValue(mi)
        finally:
            self._syncing_time_filter = False

        self._update_time_summary_label()
        self.trigger_refresh()

    on_time_filter_changed = on_time_preset_changed

    def on_spin_duration_changed(self):
        if self._syncing_time_filter:
            return
        self._syncing_time_filter = True
        try:
            self.custom_date_edit.setVisible(False)
            total_sec = self.get_filter_duration_seconds()
            if total_sec == 0:
                idx = self.time_combo.findText("All Time")
                if idx >= 0:
                    self.time_combo.setCurrentIndex(idx)
            else:
                idx = self.time_combo.findText("Custom Duration")
                if idx >= 0:
                    self.time_combo.setCurrentIndex(idx)
        finally:
            self._syncing_time_filter = False

        self._update_time_summary_label()
        self.time_filter_debounce.start()

    def on_reset_time_clicked(self):
        self._syncing_time_filter = True
        try:
            self.spin_months.setValue(0)
            self.spin_weeks.setValue(0)
            self.spin_days.setValue(0)
            self.spin_hours.setValue(0)
            self.spin_minutes.setValue(0)
            self.custom_date_edit.setVisible(False)
            idx = self.time_combo.findText("All Time")
            if idx >= 0:
                self.time_combo.setCurrentIndex(idx)
        finally:
            self._syncing_time_filter = False

        self._update_time_summary_label()
        self.trigger_refresh()

    def _update_time_summary_label(self):
        if self.time_combo.currentText() == "Specific Date...":
            date_str = self.custom_date_edit.date().toString("yyyy-MM-dd")
            self.lbl_time_summary.setText(f"📅 Day: {date_str}")
            self.lbl_time_summary.setToolTip(f"Specific date filter: {date_str}")
            self.lbl_time_summary.setStyleSheet("""
                color: #f59e0b; background-color: #1c1917; border: 1px solid #78350f;
                border-radius: 4px; padding: 3px 8px; font-size: 11px; font-weight: 700;
            """)
            return

        m = self.spin_months.value()
        w = self.spin_weeks.value()
        d = self.spin_days.value()
        h = self.spin_hours.value()
        mi = self.spin_minutes.value()

        total_minutes = (m * 30 * 1440) + (w * 7 * 1440) + (d * 1440) + (h * 60) + mi
        if total_minutes == 0:
            self.lbl_time_summary.setText("🌐 All Time")
            self.lbl_time_summary.setToolTip("Showing all recorded network history")
            self.lbl_time_summary.setStyleSheet("""
                color: #94a3b8; background-color: #0f172a; border: 1px solid #1e293b;
                border-radius: 4px; padding: 3px 8px; font-size: 11px; font-weight: 600;
            """)
            return

        total_days = total_minutes // 1440
        rem_min_in_day = total_minutes % 1440
        rem_hours = rem_min_in_day // 60
        final_mins = rem_min_in_day % 60
        total_hours = total_minutes // 60

        if total_days > 0 and rem_hours == 0 and final_mins == 0:
            text = f"📅 Last {total_days} Day{'s' if total_days != 1 else ''}"
        elif total_hours > 0 and final_mins == 0:
            text = f"🕒 Last {total_hours} Hour{'s' if total_hours != 1 else ''}"
        elif total_hours > 0 and final_mins > 0:
            text = f"🕒 Last {total_hours}h {final_mins}m"
        else:
            text = f"🕒 Last {final_mins} Minute{'s' if final_mins != 1 else ''}"

        detailed_parts = []
        if m > 0: detailed_parts.append(f"{m} mo")
        if w > 0: detailed_parts.append(f"{w} wk")
        if d > 0: detailed_parts.append(f"{d} d")
        if h > 0: detailed_parts.append(f"{h} hr")
        if mi > 0: detailed_parts.append(f"{mi} min")
        detail_str = " + ".join(detailed_parts)

        self.lbl_time_summary.setText(text)
        self.lbl_time_summary.setToolTip(f"Active Filter: {detail_str} (= {total_minutes:,} minutes total)")
        self.lbl_time_summary.setStyleSheet("""
            color: #38bdf8; background-color: #0c4a6e; border: 1px solid #0284c7;
            border-radius: 4px; padding: 3px 8px; font-size: 11px; font-weight: 700;
        """)

    def get_time_sql_clause(self):
        filter_text = self.time_combo.currentText()
        if filter_text == "Specific Date...":
            date_str = self.custom_date_edit.date().toString("yyyy-MM-dd")
            return f"WHERE date(timestamp) = date('{date_str}')"

        sec = self.get_filter_duration_seconds()
        if sec > 0:
            return f"WHERE timestamp >= datetime('now', '-{sec} seconds', 'localtime')"
        return ""

    def trigger_refresh(self):
        """Dispatches an asynchronous database query worker thread."""
        if self.worker and self.worker.isRunning():
            self._pending_refresh = True
            return

        self._pending_refresh = False
        time_clause = self.get_time_sql_clause()
        self.worker = DbQueryWorker(self.db_path, time_clause, self.group_by_mode, self.timeline_hours, self)
        self.worker.data_loaded.connect(self.on_data_loaded)
        self.worker.start()

    @Slot(list, dict, list, list, list, dict, dict, list, bool)
    def on_data_loaded(self, raw_data, totals, timeline_pts, day_items, hour_items, children_map, day_children_map, top_apps, daemon_active):
        """Callback executed on UI thread when background query completes."""
        # 1. Daemon status
        if daemon_active:
            self.status_badge.setText("● Daemon Active")
            self.status_badge.setStyleSheet("background-color: #064e3b; color: #34d399; border: 1px solid #059669;")
        else:
            self.status_badge.setText("○ Daemon Inactive")
            self.status_badge.setStyleSheet("background-color: #450a0a; color: #f87171; border: 1px solid #dc2626;")

        self.raw_data = raw_data
        self.totals = totals

        # 2. Speed Calculation
        now = time.time()
        curr_rx = totals.get("wan_down", 0) + totals.get("lan_down", 0)
        curr_tx = totals.get("wan_up", 0) + totals.get("lan_up", 0)

        if self.prev_check_time and self.prev_rx_bytes is not None:
            dt = max(0.2, now - self.prev_check_time)
            rx_diff = max(0, curr_rx - self.prev_rx_bytes)
            tx_diff = max(0, curr_tx - self.prev_tx_bytes)
            self.curr_rx_speed = rx_diff / dt
            self.curr_tx_speed = tx_diff / dt

        self.prev_check_time = now
        self.prev_rx_bytes = curr_rx
        self.prev_tx_bytes = curr_tx

        # 3. Update Bottom Panel Metrics
        tot_bytes = totals.get("grand_total", 0)
        wan_tot = totals.get("wan_tot", 0)
        lan_tot = totals.get("lan_tot", 0)
        max_wl = max(wan_tot, lan_tot, 1)

        self.lbl_down_val.setText(f"{format_bytes(curr_rx)}  ↓")
        self.lbl_down_speed.setText(format_speed(self.curr_rx_speed))

        self.lbl_up_val.setText(f"{format_bytes(curr_tx)}  ↑")
        self.lbl_up_speed.setText(format_speed(self.curr_tx_speed))

        self.arc_gauge.set_total(format_bytes(tot_bytes))

        self.lbl_wan_num.setText(format_bytes(wan_tot))
        self.bar_wan.set_values(totals.get("wan_down", 0), totals.get("wan_up", 0), max_wl)

        self.lbl_lan_num.setText(format_bytes(lan_tot))
        self.bar_lan.set_values(totals.get("lan_down", 0), totals.get("lan_up", 0), max_wl)

        # 4. Update Timeline Wave Chart
        self.timeline_chart.set_data(timeline_pts, self.timeline_hours)

        # 5. Update GlassWire Cards View with expandable child applications
        self.cards_view.populate(
            self.raw_data,
            day_items,
            hour_items,
            totals,
            group_by_mode=self.group_by_mode,
            children_map=children_map,
            day_children_map=day_children_map,
            top_apps=top_apps
        )

        # 6. Apply filters & update table in-place
        self.apply_client_filters()

        if getattr(self, "_pending_refresh", False):
            self._pending_refresh = False
            self.trigger_refresh()

    def get_min_bytes_filter(self):
        sel = self.min_size_combo.currentText()
        if "100 KB" in sel: return 100 * 1024
        if "1 MB" in sel:   return 1 * 1024 * 1024
        if "10 MB" in sel:  return 10 * 1024 * 1024
        if "100 MB" in sel: return 100 * 1024 * 1024
        if "500 MB" in sel: return 500 * 1024 * 1024
        if "1 GB" in sel:   return 1024 * 1024 * 1024
        return 0

    def apply_client_filters(self):
        search_kw = self.search_input.text().strip().lower()
        min_bytes = self.get_min_bytes_filter()

        res = []
        for item in self.raw_data:
            if search_kw and search_kw not in item["entity"].lower():
                continue
            if item["total"] < min_bytes:
                continue
            res.append(item)

        self.filtered_data = res
        self.update_table_in_place()

    def update_table_in_place(self):
        """Updates QTableWidget items in-place with 10 explicit columns."""
        header = self.table.horizontalHeader()
        sort_col = header.sortIndicatorSection()
        sort_order = header.sortIndicatorOrder()

        self.table.setSortingEnabled(False)
        self.table.setRowCount(len(self.filtered_data))

        for row_idx, item in enumerate(self.filtered_data):
            # Col 0: Entity (App or Period)
            e_item = self.table.item(row_idx, 0)
            if not e_item:
                e_item = QTableWidgetItem(f"  {item['entity']}")
                e_item.setFont(QFont("sans-serif", 10, QFont.Weight.Bold))
                self.table.setItem(row_idx, 0, e_item)
            else:
                e_item.setText(f"  {item['entity']}")

            # Col 1: Total WAN
            w_tot_text = format_bytes(item["total_wan"])
            w_tot_item = self.table.item(row_idx, 1)
            if not w_tot_item:
                w_tot_item = NumericTableWidgetItem(w_tot_text, item["total_wan"])
                w_tot_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                w_tot_item.setFont(QFont("sans-serif", 9, QFont.Weight.Bold))
                w_tot_item.setForeground(QColor("#06b6d4"))
                self.table.setItem(row_idx, 1, w_tot_item)
            else:
                w_tot_item.setText(w_tot_text)
                w_tot_item.sort_val = item["total_wan"]

            # Col 2: WAN Down
            w_down_text = format_bytes(item["wan_down"])
            w_down_item = self.table.item(row_idx, 2)
            if not w_down_item:
                w_down_item = NumericTableWidgetItem(w_down_text, item["wan_down"])
                w_down_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                w_down_item.setForeground(QColor("#fbbf24"))
                self.table.setItem(row_idx, 2, w_down_item)
            else:
                w_down_item.setText(w_down_text)
                w_down_item.sort_val = item["wan_down"]

            # Col 3: WAN Up
            w_up_text = format_bytes(item["wan_up"])
            w_up_item = self.table.item(row_idx, 3)
            if not w_up_item:
                w_up_item = NumericTableWidgetItem(w_up_text, item["wan_up"])
                w_up_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                w_up_item.setForeground(QColor("#f59e0b"))
                self.table.setItem(row_idx, 3, w_up_item)
            else:
                w_up_item.setText(w_up_text)
                w_up_item.sort_val = item["wan_up"]

            # Col 4: Total LAN
            l_tot_text = format_bytes(item["total_lan"])
            l_tot_item = self.table.item(row_idx, 4)
            if not l_tot_item:
                l_tot_item = NumericTableWidgetItem(l_tot_text, item["total_lan"])
                l_tot_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                l_tot_item.setFont(QFont("sans-serif", 9, QFont.Weight.Bold))
                l_tot_item.setForeground(QColor("#c084fc"))
                self.table.setItem(row_idx, 4, l_tot_item)
            else:
                l_tot_item.setText(l_tot_text)
                l_tot_item.sort_val = item["total_lan"]

            # Col 5: LAN Down
            l_down_text = format_bytes(item["lan_down"])
            l_down_item = self.table.item(row_idx, 5)
            if not l_down_item:
                l_down_item = NumericTableWidgetItem(l_down_text, item["lan_down"])
                l_down_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                l_down_item.setForeground(QColor("#a855f7"))
                self.table.setItem(row_idx, 5, l_down_item)
            else:
                l_down_item.setText(l_down_text)
                l_down_item.sort_val = item["lan_down"]

            # Col 6: LAN Up
            l_up_text = format_bytes(item["lan_up"])
            l_up_item = self.table.item(row_idx, 6)
            if not l_up_item:
                l_up_item = NumericTableWidgetItem(l_up_text, item["lan_up"])
                l_up_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                l_up_item.setForeground(QColor("#9333ea"))
                self.table.setItem(row_idx, 6, l_up_item)
            else:
                l_up_item.setText(l_up_text)
                l_up_item.sort_val = item["lan_up"]

            # Col 7: Grand Total
            tot_text = format_bytes(item["total"])
            tot_item = self.table.item(row_idx, 7)
            if not tot_item:
                tot_item = NumericTableWidgetItem(tot_text, item["total"])
                tot_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                tot_item.setFont(QFont("sans-serif", 10, QFont.Weight.Bold))
                tot_item.setForeground(QColor("#10b981"))
                self.table.setItem(row_idx, 7, tot_item)
            else:
                tot_item.setText(tot_text)
                tot_item.sort_val = item["total"]

            # Col 8: Samples
            s_text = f"{item['samples']:,}"
            s_item = self.table.item(row_idx, 8)
            if not s_item:
                s_item = NumericTableWidgetItem(s_text, item["samples"])
                s_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter | Qt.AlignmentFlag.AlignVCenter)
                s_item.setForeground(QColor("#64748b"))
                self.table.setItem(row_idx, 8, s_item)
            else:
                s_item.setText(s_text)
                s_item.sort_val = item["samples"]

            # Col 9: Last Seen
            last_item = self.table.item(row_idx, 9)
            if not last_item:
                last_item = QTableWidgetItem(item["last_seen"])
                last_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter | Qt.AlignmentFlag.AlignVCenter)
                last_item.setForeground(QColor("#94a3b8"))
                self.table.setItem(row_idx, 9, last_item)
            else:
                last_item.setText(item["last_seen"])

        self.table.sortItems(sort_col, sort_order)
        self.table.setSortingEnabled(True)

    def on_table_context_menu(self, pos):
        item = self.table.itemAt(pos)
        if not item: return

        row = item.row()
        ent_item = self.table.item(row, 0)
        if not ent_item: return

        ent_name = ent_item.text().strip().replace("📅 ", "").replace("🕒 ", "").replace("📆 ", "").replace("🗓️ ", "")

        menu = QMenu(self)
        copy_action = menu.addAction(f"📋 Copy '{ent_name}'")
        filter_action = menu.addAction(f"🔍 Filter by '{ent_name}'")
        menu.addSeparator()
        reset_filter_action = menu.addAction("❌ Clear Search Filters")

        action = menu.exec(self.table.viewport().mapToGlobal(pos))
        if action == copy_action:
            QApplication.clipboard().setText(ent_name)
        elif action == filter_action:
            self.search_input.setText(ent_name)
        elif action == reset_filter_action:
            self.search_input.clear()
            self.min_size_combo.setCurrentIndex(0)

    def export_to_csv(self):
        if not self.filtered_data:
            QMessageBox.information(self, "Export", "No data available to export.")
            return

        filename, _ = QFileDialog.getSaveFileName(
            self, "Export Network Usage to CSV",
            f"netmonitor_{self.group_by_mode.lower()}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
            "CSV Files (*.csv)"
        )
        if not filename:
            return

        try:
            with open(filename, "w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                writer.writerow([
                    "Entity",
                    "Total WAN (Bytes)", "Total WAN (Formatted)",
                    "WAN Download (Bytes)", "WAN Download (Formatted)",
                    "WAN Upload (Bytes)", "WAN Upload (Formatted)",
                    "Total LAN (Bytes)", "Total LAN (Formatted)",
                    "LAN Download (Bytes)", "LAN Download (Formatted)",
                    "LAN Upload (Bytes)", "LAN Upload (Formatted)",
                    "Grand Total (Bytes)", "Grand Total (Formatted)",
                    "Samples", "Last Seen"
                ])
                for r in self.filtered_data:
                    writer.writerow([
                        r["entity"],
                        r["total_wan"], format_bytes(r["total_wan"]),
                        r["wan_down"], format_bytes(r["wan_down"]),
                        r["wan_up"], format_bytes(r["wan_up"]),
                        r["total_lan"], format_bytes(r["total_lan"]),
                        r["lan_down"], format_bytes(r["lan_down"]),
                        r["lan_up"], format_bytes(r["lan_up"]),
                        r["total"], format_bytes(r["total"]),
                        r["samples"], r["last_seen"]
                    ])
            QMessageBox.information(self, "Export Successful", f"Saved {len(self.filtered_data)} records to:\n{filename}")
        except Exception as e:
            QMessageBox.critical(self, "Export Failed", f"Could not write CSV:\n{str(e)}")

    def on_clear_db_clicked(self):
        dlg = DatabasePurgeDialog(self.db_path, self)
        if dlg.exec():
            self.trigger_refresh()


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("NetMonitor")
    app.setApplicationDisplayName("NetMonitor")
    app.setOrganizationName("ii3Bdallh")

    icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "netmon.png")
    if os.path.exists(icon_path):
        app.setWindowIcon(QIcon(icon_path))

    window = NetMonitorApp()
    window.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
