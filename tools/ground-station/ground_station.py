#!/usr/bin/env python3
"""Real-time viewer for the drone telemetry broadcast over UDP.

Listens to the telemetry datagrams emitted by the flight process and plots the
body angular rates as they arrive. Packet decoding lives in telemetry_wire.py
so it stays testable without a GUI.

Run with --help for the available options.
"""

import argparse
import errno
import socket
import sys
from collections import deque
from typing import Deque, List

import pyqtgraph as pg
from PyQt6 import QtCore, QtWidgets

from telemetry_wire import TELEMETRY_PORT, TELEMETRY_PACKET_SIZE, decode_telemetry

#: Plot refresh period, in milliseconds (roughly 30 Hz).
REFRESH_PERIOD_MS = 33

#: Expected telemetry rate, used to size the history buffers.
EXPECTED_RATE_HZ = 100.0

#: Extra room on top of window * rate, so a faster sender cannot starve the plot.
BUFFER_MARGIN = 4.0

#: Smallest history buffer, whatever the requested window.
MIN_BUFFER_SAMPLES = 1024

#: Receive buffer, large enough for any single datagram.
RECV_BUFFER_SIZE = 2048

#: Curve names and colors, one per gyro axis.
GYRO_CURVES = (
    ("gyro x", "#e6194b"),
    ("gyro y", "#3cb44b"),
    ("gyro z", "#4363d8"),
)

#: Default seconds of history kept on screen.
DEFAULT_WINDOW_S = 10.0


def open_socket(port: int) -> socket.socket:
    """Bind a nonblocking UDP socket to every interface on the given port.

    SO_REUSEADDR lets several tools listen to the same broadcast at once.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.setblocking(False)
    return sock


class GyroPlotWidget(pg.PlotWidget):
    """Plots the three gyro axes against time, fed by a periodic timer."""

    def __init__(self, sock: socket.socket, window_s: float) -> None:
        super().__init__()

        self._socket = sock
        self._window_s = window_s
        self._received = 0
        self._dropped = 0
        self._first_timestamp_us = None

        capacity = max(
            MIN_BUFFER_SAMPLES,
            int(window_s * EXPECTED_RATE_HZ * BUFFER_MARGIN),
        )
        self._times: Deque[float] = deque(maxlen=capacity)
        self._gyro: List[Deque[float]] = [deque(maxlen=capacity) for _ in GYRO_CURVES]

        self.setBackground("w")
        self.showGrid(x=True, y=True, alpha=0.3)
        self.setLabel("bottom", "time", units="s")
        self.setLabel("left", "angular rate", units="rad/s")
        self.addLegend()
        self._curves = [
            self.plot(name=name, pen=pg.mkPen(color, width=2))
            for name, color in GYRO_CURVES
        ]

        self._refresh_title()

        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._on_tick)
        self._timer.start(REFRESH_PERIOD_MS)

    def _on_tick(self) -> None:
        """Drain every pending datagram, then redraw."""
        if self._drain_socket():
            self._refresh_plot()
        self._refresh_title()

    def _drain_socket(self) -> bool:
        """Read until the socket is empty. Returns True if anything was stored."""
        appended = False
        while True:
            try:
                datagram = self._socket.recv(RECV_BUFFER_SIZE)
            except BlockingIOError:
                return appended
            except OSError as error:
                if error.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
                    return appended
                raise

            sample = decode_telemetry(datagram)
            if sample is None:
                self._dropped += 1
                continue

            self._received += 1
            if self._first_timestamp_us is None:
                self._first_timestamp_us = sample.timestamp_us

            elapsed_s = (sample.timestamp_us - self._first_timestamp_us) * 1e-6
            self._times.append(elapsed_s)
            for axis, buffer in enumerate(self._gyro):
                buffer.append(sample.gyro_rad_s[axis])
            appended = True

    def _refresh_plot(self) -> None:
        """Push the buffers to the curves and slide the visible time window."""
        times = list(self._times)
        for curve, buffer in zip(self._curves, self._gyro):
            curve.setData(times, list(buffer))

        latest = times[-1]
        self.setXRange(max(0.0, latest - self._window_s), max(latest, self._window_s))

    def _refresh_title(self) -> None:
        """Show the packet counters in the window title."""
        window = self.window()
        if window is not None:
            window.setWindowTitle(
                "Ground station - received {} - dropped {}".format(
                    self._received, self._dropped
                )
            )


def parse_arguments(argv: List[str]) -> argparse.Namespace:
    """Parse the command line."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--port",
        type=int,
        default=TELEMETRY_PORT,
        help="UDP port to listen on (default: %(default)s)",
    )
    parser.add_argument(
        "--window",
        type=float,
        default=DEFAULT_WINDOW_S,
        help="seconds of history shown (default: %(default)s)",
    )
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    """Open the socket, build the window, run the Qt loop."""
    args = parse_arguments(argv)
    if args.window <= 0.0:
        print("--window must be strictly positive", file=sys.stderr)
        return 2

    try:
        sock = open_socket(args.port)
    except OSError as error:
        print("cannot bind UDP port {}: {}".format(args.port, error), file=sys.stderr)
        return 1

    application = QtWidgets.QApplication(sys.argv[:1])
    widget = GyroPlotWidget(sock, args.window)
    widget.resize(900, 500)
    widget.show()
    print(
        "listening on 0.0.0.0:{}, expecting {} byte packets".format(
            args.port, TELEMETRY_PACKET_SIZE
        )
    )
    return application.exec()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
