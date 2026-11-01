#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Live GNSS plotter for the PiggySat test tool.

Connects to the board through a J-Link (pylink), opens RTT, sends "gnss start" to the
test shell and plots the NMEA output live:

- C/N0 (SNR) per satellite, green when used in the fix
- sky plot (azimuth / elevation) coloured by C/N0
- satellites tracked / used and mean / max C/N0 over time
- fix information: UTC time, fix type, position, altitude, DOPs, speed, course

Close the window (or press Ctrl+C) to stop; "gnss stop" is sent on exit.

Usage:
    python tools/gnss_plotter.py [--serial 261009348] [--log nmea.txt]

Close J-Link RTT Viewer and other RTT clients first: two RTT readers steal each other's data.
"""

import argparse
import collections
import math
import re
import sys
import threading
import time

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from matplotlib.cm import ScalarMappable
from matplotlib.colors import Normalize
from matplotlib.patches import FancyBboxPatch, Patch
from matplotlib.ticker import MaxNLocator
import pylink

DEFAULT_DEVICE = "nRF9151_xxCA"
RTT_CHANNEL = 0
HISTORY_SECONDS = 300
SAT_TIMEOUT_S = 5.0
CN0_MAX = 55

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Za-z0-9]|\x1b.")
# A sentence inside a shell line; the prompt ("piggysat:~$ ") also contains '$'.
NMEA_RE = re.compile(r"\$[A-Z]{5},[^$*\r\n]*\*[0-9A-Fa-f]{2}")

FIX_TYPES = {1: "no fix", 2: "2D", 3: "3D"}
GGA_QUALITY = {0: "invalid", 1: "GPS", 2: "DGPS", 4: "RTK fixed", 5: "RTK float", 6: "estimated"}


def constellation(talker, prn):
    """Short constellation name from the NMEA talker ID and PRN."""
    if 193 <= prn <= 202:
        return "QZSS"
    return {"GP": "GPS", "GL": "GLONASS", "GA": "Galileo", "GB": "BeiDou", "BD": "BeiDou",
            "GQ": "QZSS", "GN": "GNSS"}.get(talker, talker)


def checksum_ok(sentence):
    """Validate "$...*hh"."""
    if "*" not in sentence:
        return False
    body, _, given = sentence[1:].partition("*")
    calc = 0
    for ch in body:
        calc ^= ord(ch)
    try:
        return calc == int(given[:2], 16)
    except ValueError:
        return False


def to_int(value, default=None):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def to_float(value, default=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def nmea_coord(value, hemisphere):
    """ddmm.mmmm / dddmm.mmmm + N/S/E/W -> signed decimal degrees."""
    if not value:
        return None
    raw = float(value)
    degrees = int(raw // 100)
    decimal = degrees + (raw - degrees * 100) / 60.0
    return -decimal if hemisphere in ("S", "W") else decimal


def nmea_time(value):
    if not value or len(value) < 6:
        return None
    return f"{value[0:2]}:{value[2:4]}:{value[4:]}"


class GnssState:
    """Everything decoded from the NMEA stream, shared between reader and plotter."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sats = {}            # (talker, prn) -> {elev, azim, cn0, seen}
        self.gsv_pending = {}     # talker -> list of satellites being assembled
        self.used = set()         # PRNs used in the fix (GSA)
        self.fix = {}             # decoded GGA / GSA / RMC fields
        self.history = collections.deque()
        self.sentences = 0
        self.bad_checksums = 0
        self.last_rx = None
        self.messages = collections.deque(maxlen=4)

    # NMEA handlers ---------------------------------------------------------

    def handle(self, sentence):
        fields = sentence[1:sentence.index("*")].split(",")
        talker, kind = fields[0][:2], fields[0][2:]
        handler = getattr(self, f"_on_{kind.lower()}", None)
        if handler is not None:
            handler(talker, fields)
        self.sentences += 1
        self.last_rx = time.time()

    def _on_gsv(self, talker, f):
        total, number = to_int(f[1], 1), to_int(f[2], 1)
        if number == 1:
            self.gsv_pending[talker] = []
        pending = self.gsv_pending.setdefault(talker, [])
        # Groups of 4 fields: PRN, elevation, azimuth, C/N0 (an optional signal ID may follow).
        for i in range(4, len(f) - 3, 4):
            prn = to_int(f[i])
            if prn is None:
                continue
            pending.append((prn, to_int(f[i + 1]), to_int(f[i + 2]), to_int(f[i + 3])))
        if number == total:
            now = time.time()
            for key in [k for k in self.sats if k[0] == talker]:
                del self.sats[key]
            for prn, elev, azim, cn0 in pending:
                self.sats[(talker, prn)] = {"elev": elev, "azim": azim, "cn0": cn0, "seen": now}
            self.gsv_pending[talker] = []

    def _on_gsa(self, talker, f):
        self.fix["fix_type"] = to_int(f[2], 1)
        self.used = {prn for prn in (to_int(x) for x in f[3:15]) if prn is not None}
        self.fix["pdop"] = to_float(f[15]) if len(f) > 15 else None
        self.fix["hdop_gsa"] = to_float(f[16]) if len(f) > 16 else None
        self.fix["vdop"] = to_float(f[17]) if len(f) > 17 else None

    def _on_gga(self, talker, f):
        self.fix.update({
            "time": nmea_time(f[1]),
            "lat": nmea_coord(f[2], f[3]),
            "lon": nmea_coord(f[4], f[5]),
            "quality": to_int(f[6], 0),
            "num_used": to_int(f[7], 0),
            "hdop": to_float(f[8]),
            "alt": to_float(f[9]),
            "geoid": to_float(f[11]) if len(f) > 11 else None,
        })
        self._sample_history()

    def _on_rmc(self, talker, f):
        self.fix.update({
            "status": f[2],
            "speed_kn": to_float(f[7]),
            "course": to_float(f[8]),
            "date": f"20{f[9][4:6]}-{f[9][2:4]}-{f[9][0:2]}" if len(f[9]) == 6 else None,
        })

    def _on_gll(self, talker, f):
        pass  # same position as GGA

    def _sample_history(self):
        now = time.time()
        cn0 = [s["cn0"] for s in self.sats.values() if s["cn0"]]
        self.history.append((now, len(self.sats), len(self.used),
                             sum(cn0) / len(cn0) if cn0 else None, max(cn0) if cn0 else None))
        while self.history and now - self.history[0][0] > HISTORY_SECONDS:
            self.history.popleft()


class RttLink:
    """J-Link + RTT connection running a background reader thread."""

    def __init__(self, args, state):
        self.args = args
        self.state = state
        self.jlink = pylink.JLink()
        self.stop_event = threading.Event()
        self.thread = None
        self.log = open(args.log, "a", encoding="utf-8") if args.log else None

    def open(self):
        if self.args.serial:
            self.jlink.open(serial_no=self.args.serial)
        else:
            self.jlink.open()
        self.jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
        self.jlink.connect(self.args.device, speed=self.args.speed)
        if self.jlink.halted():
            self.jlink.restart()

        self.jlink.rtt_start(self.args.rtt_address)
        deadline = time.time() + 10
        while True:
            try:
                if self.jlink.rtt_get_num_up_buffers() > 0:
                    break
            except pylink.errors.JLinkRTTException:
                pass
            if time.time() > deadline:
                raise RuntimeError("RTT control block not found (try --rtt-address 0x...)")
            time.sleep(0.1)

        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def send(self, command):
        self.jlink.rtt_write(RTT_CHANNEL, list((command + "\r").encode()))

    def close(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        try:
            self.jlink.rtt_stop()
        finally:
            self.jlink.close()
            if self.log:
                self.log.close()

    def _reader(self):
        buffer = ""
        while not self.stop_event.is_set():
            try:
                data = self.jlink.rtt_read(RTT_CHANNEL, 4096)
            except pylink.errors.JLinkException as exc:
                with self.state.lock:
                    self.state.messages.append(f"RTT error: {exc}")
                time.sleep(0.5)
                continue
            if not data:
                time.sleep(0.02)
                continue
            buffer += bytes(data).decode("utf-8", "replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                self._line(line)

    def _line(self, line):
        line = ANSI_RE.sub("", line).replace("\r", "")
        match = NMEA_RE.search(line)
        if match:
            sentence = match.group(0)
            if self.log:
                self.log.write(sentence + "\n")
            if self.args.echo:
                print(sentence)
            with self.state.lock:
                if checksum_ok(sentence):
                    self.state.handle(sentence)
                else:
                    self.state.bad_checksums += 1
            return

        # Shell feedback, e.g. "gnss: ..." or "Modem busy: ..."
        text = line.split("$ ", 1)[-1].strip()
        if "'gnss' test is running" in text:
            text = "GNSS test was already running"
        if text.startswith(("gnss:", "GNSS", "Modem", "error")) or "<err>" in text:
            print(text)
            with self.state.lock:
                self.state.messages.append(text)


class Plotter:
    """Live dashboard: C/N0 bars, sky plot, history and fix information."""

    BG = "#f4f5f7"
    CARD = "#ffffff"
    INK = "#1f2933"
    MUTED = "#7b8794"
    GRID = "#e4e7eb"
    BLUE = "#3d7dd8"
    GREEN = "#2f9e44"
    ORANGE = "#f08c00"
    RED = "#e03131"
    CN0_LOW, CN0_HIGH = 15, 50

    def __init__(self, state, title):
        self.state = state
        plt.rcParams.update({
            "font.family": "DejaVu Sans",
            "font.size": 9,
            "axes.edgecolor": self.GRID,
            "axes.labelcolor": self.MUTED,
            "axes.titlecolor": self.INK,
            "axes.titlesize": 11,
            "axes.titleweight": "bold",
            "axes.titlelocation": "left",
            "axes.spines.top": False,
            "axes.spines.right": False,
            "xtick.color": self.MUTED,
            "ytick.color": self.MUTED,
        })
        self.cmap = plt.get_cmap("RdYlGn")
        self.norm = Normalize(self.CN0_LOW, self.CN0_HIGH)

        self.fig = plt.figure(figsize=(15, 8.5), facecolor=self.BG)
        self.fig.canvas.manager.set_window_title(title)
        grid = self.fig.add_gridspec(3, 3, width_ratios=[2.3, 1.55, 1.3],
                                     height_ratios=[1.4, 0.75, 0.75],
                                     left=0.05, right=0.985, top=0.88, bottom=0.07,
                                     wspace=0.28, hspace=0.55)
        self.ax_cn0 = self.fig.add_subplot(grid[0, 0])
        self.ax_sky = self.fig.add_subplot(grid[0, 1], projection="polar")
        self.ax_sats = self.fig.add_subplot(grid[1, 0:2])
        self.ax_hist = self.fig.add_subplot(grid[2, 0:2], sharex=self.ax_sats)
        self.ax_info = self.fig.add_subplot(grid[:, 2])

        colorbar = self.fig.colorbar(ScalarMappable(norm=self.norm, cmap=self.cmap), ax=self.ax_cn0,
                                     fraction=0.025, pad=0.015, aspect=25)
        colorbar.set_label("C/N0 [dB-Hz]", color=self.MUTED)
        colorbar.outline.set_visible(False)

        title = self.fig.text(0.05, 0.955, "PiggySat", fontsize=18, fontweight="bold", color=self.INK)
        self.fig.canvas.draw()
        right = title.get_window_extent().transformed(self.fig.transFigure.inverted()).x1
        self.fig.text(right + 0.008, 0.957, "GNSS monitor", fontsize=14, color=self.MUTED)
        self.status = self.fig.text(0.985, 0.955, "", fontsize=10, ha="right", fontweight="bold")

    def snapshot(self):
        with self.state.lock:
            now = time.time()
            sats = {k: dict(v) for k, v in self.state.sats.items() if now - v["seen"] < SAT_TIMEOUT_S}
            return (sats, set(self.state.used), dict(self.state.fix), list(self.state.history),
                    self.state.sentences, self.state.bad_checksums, self.state.last_rx,
                    list(self.state.messages))

    def update(self, _frame):
        sats, used, fix, history, sentences, bad, last_rx, messages = self.snapshot()
        ordered = sorted(sats.items(), key=lambda kv: (kv[0][0], kv[0][1]))
        self._draw_status(sentences, last_rx)
        self._draw_cn0(ordered, used)
        self._draw_sky(ordered, used)
        self._draw_history(history)
        self._draw_info(fix, sats, used, sentences, bad, last_rx, messages)

    def _cn0_color(self, cn0):
        return self.cmap(self.norm(cn0)) if cn0 else "#ced4da"

    def _draw_status(self, sentences, last_rx):
        if last_rx is None:
            self.status.set_text("● waiting for NMEA…")
            self.status.set_color(self.ORANGE)
        elif time.time() - last_rx > 3:
            self.status.set_text(f"● no data for {time.time() - last_rx:.0f} s")
            self.status.set_color(self.RED)
        else:
            self.status.set_text(f"● RTT live · {sentences} sentences")
            self.status.set_color(self.GREEN)

    def _draw_cn0(self, ordered, used):
        ax = self.ax_cn0
        ax.clear()
        ax.set_facecolor(self.CARD)
        values = [s["cn0"] or 0 for _, s in ordered]
        for i, ((talker, prn), sat) in enumerate(ordered):
            cn0 = sat["cn0"] or 0
            in_fix = prn in used
            ax.bar(i, cn0, width=0.72, color=self._cn0_color(cn0),
                   alpha=1.0 if in_fix else 0.45, hatch=None if in_fix else "////",
                   edgecolor=self.INK if in_fix else "#adb5bd", linewidth=0.8, zorder=3)
            ax.text(i, cn0 + 0.8, str(cn0) if cn0 else "–", ha="center", va="bottom",
                    fontsize=8, color=self.INK if cn0 else self.MUTED, fontweight="bold")
        ax.set_xticks(range(len(ordered)))
        ax.set_xticklabels([f"{prn}\n{constellation(t, prn)}" for (t, prn), _ in ordered], fontsize=8)
        ax.set_xlim(-0.6, max(len(ordered), 6) - 0.4)
        ax.set_ylim(0, CN0_MAX)
        ax.set_ylabel("C/N0 [dB-Hz]")
        ax.axhline(30, color=self.MUTED, linewidth=0.8, linestyle=(0, (4, 3)), zorder=2)
        ax.text(ax.get_xlim()[1], 30.6, "30 dB-Hz", fontsize=7, color=self.MUTED, ha="right")
        ax.grid(axis="y", color=self.GRID, zorder=0)
        used_n = sum(1 for (_, prn), _ in ordered if prn in used)
        ax.set_title(f"Signal strength   {len(ordered)} tracked · {used_n} used")
        ax.legend(handles=[Patch(facecolor="#69b34c", edgecolor=self.INK, label="used in fix"),
                           Patch(facecolor="#69b34c", alpha=0.45, hatch="////", edgecolor="#adb5bd",
                                 label="tracked only")],
                  loc="upper right", fontsize=8, frameon=False, ncol=2)
        if not values:
            ax.text(0.5, 0.5, "no satellites tracked yet", transform=ax.transAxes,
                    ha="center", va="center", color=self.MUTED, fontsize=11)

    def _draw_sky(self, ordered, used):
        ax = self.ax_sky
        ax.clear()
        ax.set_facecolor(self.CARD)
        ax.set_theta_zero_location("N")
        ax.set_theta_direction(-1)
        ax.set_ylim(0, 90)
        ax.set_yticks([30, 60, 90])
        ax.set_yticklabels(["60°", "30°", "0°"], fontsize=7, color=self.MUTED)
        ax.set_rlabel_position(157.5)
        ax.set_xticks([math.radians(a) for a in range(0, 360, 45)])
        ax.set_xticklabels(["N", "NE", "E", "SE", "S", "SW", "W", "NW"], color=self.INK)
        ax.grid(color=self.GRID)
        ax.set_title("Sky plot", pad=14)
        for (talker, prn), sat in ordered:
            if sat["elev"] is None or sat["azim"] is None:
                continue
            theta, r = math.radians(sat["azim"]), 90 - sat["elev"]
            in_fix = prn in used
            ax.scatter(theta, r, s=320, color=self._cn0_color(sat["cn0"]),
                       edgecolors=self.INK if in_fix else "white", linewidths=1.8 if in_fix else 1.0,
                       alpha=1.0 if in_fix else 0.6, zorder=3)
            ax.annotate(str(prn), (theta, r), fontsize=7.5, ha="center", va="center",
                        color=self.INK, fontweight="bold", zorder=4)

    def _draw_history(self, history):
        ax_s, ax_c = self.ax_sats, self.ax_hist
        ax_s.clear()
        ax_c.clear()
        for ax in (ax_s, ax_c):
            ax.set_facecolor(self.CARD)
            ax.grid(color=self.GRID)

        span = 60
        if history:
            t0 = history[-1][0]
            t = [h[0] - t0 for h in history]
            span = min(HISTORY_SECONDS, max(60, -t[0]))
            tracked = [h[1] for h in history]
            used = [h[2] for h in history]
            ax_s.fill_between(t, tracked, step="post", color=self.BLUE, alpha=0.18)
            ax_s.step(t, tracked, where="post", color=self.BLUE, linewidth=1.4, label="tracked")
            ax_s.fill_between(t, used, step="post", color=self.GREEN, alpha=0.25)
            ax_s.step(t, used, where="post", color=self.GREEN, linewidth=1.4, label="used in fix")
            ax_s.set_ylim(0, max(12, max(tracked) + 1))

            mean = [h[3] if h[3] is not None else float("nan") for h in history]
            peak = [h[4] if h[4] is not None else float("nan") for h in history]
            ax_c.fill_between(t, mean, peak, color=self.ORANGE, alpha=0.15, linewidth=0)
            ax_c.plot(t, peak, color=self.RED, linewidth=1.2, label="max")
            ax_c.plot(t, mean, color=self.ORANGE, linewidth=1.6, label="mean")
            ax_s.legend(loc="upper left", fontsize=8, frameon=False, ncol=2)
            ax_c.legend(loc="upper left", fontsize=8, frameon=False, ncol=2)
        else:
            ax_s.set_ylim(0, 12)

        ax_s.set_xlim(-span, 0)
        ax_s.yaxis.set_major_locator(MaxNLocator(integer=True))
        ax_s.set_title("Satellites", fontsize=10)
        ax_s.tick_params(labelbottom=False)
        ax_c.set_ylim(0, CN0_MAX)
        ax_c.set_title("C/N0 [dB-Hz]", fontsize=10)
        ax_c.set_xlabel("time [s]")

    def _draw_info(self, fix, sats, used, sentences, bad, last_rx, messages):
        ax = self.ax_info
        ax.clear()
        ax.axis("off")
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)

        def fmt(value, spec="", unit=""):
            return "–" if value is None else f"{value:{spec}}{unit}"

        row_step = 0.034

        def card(top, title, rows):
            height = 0.07 + len(rows) * row_step
            ax.add_patch(FancyBboxPatch((0.0, top - height), 1.0, height,
                                        boxstyle="round,pad=0,rounding_size=0.025",
                                        facecolor=self.CARD, edgecolor=self.GRID, linewidth=1))
            ax.text(0.05, top - 0.03, title.upper(), fontsize=8, color=self.MUTED,
                    fontweight="bold", va="top")
            y = top - 0.075
            for label, value in rows:
                ax.text(0.05, y, label, fontsize=9, color=self.MUTED, va="top")
                ax.text(0.95, y, value, fontsize=9.5, color=self.INK, va="top", ha="right",
                        family="monospace")
                y -= row_step
            return top - height - 0.02

        fix_type = fix.get("fix_type")
        badge = {3: ("3D FIX", self.GREEN), 2: ("2D FIX", self.ORANGE)}.get(fix_type, ("NO FIX", self.RED))
        ax.add_patch(FancyBboxPatch((0.0, 0.905), 1.0, 0.095, boxstyle="round,pad=0,rounding_size=0.025",
                                    facecolor=badge[1], edgecolor="none"))
        ax.text(0.5, 0.952, badge[0], fontsize=16, fontweight="bold", color="white",
                ha="center", va="center")

        lat, lon = fix.get("lat"), fix.get("lon")
        speed = fix.get("speed_kn")
        cn0 = [s["cn0"] for s in sats.values() if s["cn0"]]
        age = "–" if last_rx is None else f"{time.time() - last_rx:.1f} s"

        y = 0.88
        y = card(y, "Time", [
            ("Date", fix.get("date") or "–"),
            ("UTC", fix.get("time") or "–"),
        ])
        y = card(y, "Position", [
            ("Latitude", "–" if lat is None else f"{abs(lat):.6f}° {'N' if lat >= 0 else 'S'}"),
            ("Longitude", "–" if lon is None else f"{abs(lon):.6f}° {'E' if lon >= 0 else 'W'}"),
            ("Altitude", fmt(fix.get("alt"), ".1f", " m")),
            ("Speed", "–" if speed is None else f"{speed * 1.852:.1f} km/h"),
            ("Course", fmt(fix.get("course"), ".1f", "°")),
        ])
        y = card(y, "Quality", [
            ("Tracked / used", f"{len(sats)} / {len(used)}"),
            ("HDOP / PDOP", f"{fmt(fix.get('hdop'))} / {fmt(fix.get('pdop'))}"),
            ("VDOP", fmt(fix.get("vdop"))),
            ("C/N0 mean / max", "–" if not cn0 else f"{sum(cn0) / len(cn0):.1f} / {max(cn0)}"),
        ])
        y = card(y, "Stream", [
            ("Sentences", f"{sentences}  ({bad} bad)"),
            ("Last sentence", age),
        ])
        if messages:
            ax.text(0.02, y - 0.005, "BOARD", fontsize=8, color=self.MUTED, fontweight="bold", va="top")
            for i, msg in enumerate(messages[-3:]):
                ax.text(0.02, y - 0.04 - i * 0.033, msg[:48], fontsize=8, color=self.INK, va="top")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", type=int, help="J-Link serial number (default: the only one)")
    parser.add_argument("--device", default=DEFAULT_DEVICE, help=f"J-Link device (default {DEFAULT_DEVICE})")
    parser.add_argument("--speed", type=int, default=4000, help="SWD speed in kHz (default 4000)")
    parser.add_argument("--rtt-address", type=lambda v: int(v, 0),
                        help="RTT control block address (_SEGGER_RTT in zephyr.map), if auto-detect fails")
    parser.add_argument("--no-start", action="store_true", help="do not send 'gnss start' (already running)")
    parser.add_argument("--no-stop", action="store_true", help="do not send 'gnss stop' on exit")
    parser.add_argument("--log", help="append the raw NMEA sentences to this file")
    parser.add_argument("--echo", action="store_true", help="print the NMEA sentences to the console")
    parser.add_argument("--interval", type=int, default=1000, help="plot refresh in ms (default 1000)")
    return parser.parse_args()


def main():
    args = parse_args()
    state = GnssState()
    link = RttLink(args, state)

    try:
        link.open()
    except Exception as exc:  # pylink raises several exception types
        sys.exit(f"Cannot connect over RTT: {exc}")

    print(f"Connected to {args.device} over RTT")
    if not args.no_start:
        link.send("gnss start")

    plotter = Plotter(state, "PiggySat GNSS")
    _anim = FuncAnimation(plotter.fig, plotter.update, interval=args.interval, cache_frame_data=False)
    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    finally:
        if not args.no_stop:
            try:
                link.send("gnss stop")
                time.sleep(0.5)
            except pylink.errors.JLinkException:
                pass
        link.close()


if __name__ == "__main__":
    main()
