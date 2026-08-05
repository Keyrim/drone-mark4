#!/usr/bin/env python3
"""Real-time viewer for the drone UDP broadcasts.

Stacked plots fed by two streams:
  - body angular rates, from the telemetry broadcast of the flight process;
  - attitude as Euler angles, estimated (telemetry, solid) against the exact
    simulator state (sim raw broadcast, dashed);
  - attitude error angle between the two quaternions, the estimator's
    validation metric;
  - altitude and vertical velocity, estimated against exact.

Without a simulator the sim raw stream simply stays silent and only the
estimated curves draw. Packet decoding lives in telemetry_wire.py so it stays
testable without a GUI. Run with --help for the available options.
"""

import argparse
import errno
import socket
import sys
from collections import deque
from typing import Deque, List, Optional, Tuple

import pyqtgraph as pg
from PyQt6 import QtCore, QtWidgets

from telemetry_wire import (
    SIM_RAW_PORT,
    TELEMETRY_PORT,
    decode_sim_raw,
    decode_telemetry,
    error_angle_deg,
    euler_deg,
)

#: Plot refresh period, in milliseconds (roughly 30 Hz).
REFRESH_PERIOD_MS = 33

#: Expected sample rate, used to size the history buffers.
EXPECTED_RATE_HZ = 100.0

#: Extra room on top of window * rate, so a faster sender cannot starve the plot.
BUFFER_MARGIN = 4.0

#: Smallest history buffer, whatever the requested window.
MIN_BUFFER_SAMPLES = 1024

#: Receive buffer, large enough for any single datagram.
RECV_BUFFER_SIZE = 2048

#: Default seconds of history kept on screen.
DEFAULT_WINDOW_S = 10.0

#: Axis names and colors, shared by the gyro and the Euler angle curves.
AXIS_COLORS = (
    ("x", "#e6194b"),
    ("y", "#3cb44b"),
    ("z", "#4363d8"),
)

EULER_NAMES = ("roll", "pitch", "yaw")


def open_socket(port: int) -> socket.socket:
    """Bind a nonblocking UDP socket to every interface on the given port.

    SO_REUSEADDR lets several tools listen to the same broadcast at once.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.setblocking(False)
    return sock


def drain(sock: socket.socket) -> List[bytes]:
    """Read every pending datagram of a nonblocking socket."""
    datagrams = []
    while True:
        try:
            datagrams.append(sock.recv(RECV_BUFFER_SIZE))
        except BlockingIOError:
            return datagrams
        except OSError as error:
            if error.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
                return datagrams
            raise


class GroundStationWindow(pg.GraphicsLayoutWidget):
    """Three stacked time plots fed by the telemetry and sim raw sockets."""

    def __init__(
        self,
        telemetry_socket: socket.socket,
        sim_raw_socket: socket.socket,
        window_s: float,
    ) -> None:
        super().__init__()

        self._telemetry_socket = telemetry_socket
        self._sim_raw_socket = sim_raw_socket
        self._window_s = window_s
        self._received = 0
        self._dropped = 0
        self._first_timestamp_us: Optional[int] = None
        self._last_true_quat: Optional[Tuple[float, float, float, float]] = None

        capacity = max(
            MIN_BUFFER_SAMPLES,
            int(window_s * EXPECTED_RATE_HZ * BUFFER_MARGIN),
        )

        def buffers(count: int) -> List[Deque[float]]:
            return [deque(maxlen=capacity) for _ in range(count)]

        self._telemetry_times: Deque[float] = deque(maxlen=capacity)
        self._gyro = buffers(3)
        self._euler_estimated = buffers(3)
        self._error_deg: Deque[float] = deque(maxlen=capacity)
        self._vertical_estimated = buffers(2)  # altitude [m], velocity [m/s]
        self._sim_raw_times: Deque[float] = deque(maxlen=capacity)
        self._euler_true = buffers(3)
        self._vertical_true = buffers(2)

        self.setBackground("w")

        gyro_plot = self.addPlot(row=0, col=0)
        gyro_plot.setLabel("left", "angular rate", units="rad/s")
        gyro_plot.addLegend()
        self._gyro_curves = [
            gyro_plot.plot(name="gyro " + name, pen=pg.mkPen(color, width=2))
            for name, color in AXIS_COLORS
        ]

        euler_plot = self.addPlot(row=1, col=0)
        euler_plot.setLabel("left", "attitude", units="deg")
        euler_plot.addLegend()
        self._estimated_curves = [
            euler_plot.plot(name=name + " est", pen=pg.mkPen(color, width=2))
            for name, (_, color) in zip(EULER_NAMES, AXIS_COLORS)
        ]
        self._true_curves = [
            euler_plot.plot(
                name=name + " true",
                pen=pg.mkPen(color, width=1, style=QtCore.Qt.PenStyle.DashLine),
            )
            for name, (_, color) in zip(EULER_NAMES, AXIS_COLORS)
        ]

        error_plot = self.addPlot(row=2, col=0)
        error_plot.setLabel("left", "attitude error", units="deg")
        self._error_curve = error_plot.plot(pen=pg.mkPen("#f58231", width=2))

        vertical_plot = self.addPlot(row=3, col=0)
        vertical_plot.setLabel("left", "vertical (alt m, vz m/s)")
        vertical_plot.setLabel("bottom", "time", units="s")
        vertical_plot.addLegend()
        vertical_names = ("altitude", "vz")
        self._vertical_estimated_curves = [
            vertical_plot.plot(name=name + " est", pen=pg.mkPen(color, width=2))
            for name, (_, color) in zip(vertical_names, AXIS_COLORS)
        ]
        self._vertical_true_curves = [
            vertical_plot.plot(
                name=name + " true",
                pen=pg.mkPen(color, width=1, style=QtCore.Qt.PenStyle.DashLine),
            )
            for name, (_, color) in zip(vertical_names, AXIS_COLORS)
        ]

        self._plots = (gyro_plot, euler_plot, error_plot, vertical_plot)
        for plot in self._plots:
            plot.showGrid(x=True, y=True, alpha=0.3)
        for plot in self._plots[1:]:
            plot.setXLink(gyro_plot)

        self._refresh_title()

        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._on_tick)
        self._timer.start(REFRESH_PERIOD_MS)

    def _on_tick(self) -> None:
        """Drain both sockets, then redraw."""
        appended = self._drain_sim_raw()
        appended = self._drain_telemetry() or appended
        if appended:
            self._refresh_plot()
        self._refresh_title()

    def _elapsed_s(self, timestamp_us: int) -> float:
        """Seconds since the first sample seen, both streams share the clock."""
        if self._first_timestamp_us is None:
            self._first_timestamp_us = timestamp_us
        return (timestamp_us - self._first_timestamp_us) * 1e-6

    def _drain_sim_raw(self) -> bool:
        appended = False
        for datagram in drain(self._sim_raw_socket):
            sample = decode_sim_raw(datagram)
            if sample is None:
                self._dropped += 1
                continue
            self._received += 1
            self._last_true_quat = sample.attitude_quat
            self._sim_raw_times.append(self._elapsed_s(sample.timestamp_us))
            for axis, angle in enumerate(euler_deg(sample.attitude_quat)):
                self._euler_true[axis].append(angle)
            self._vertical_true[0].append(sample.position_m[2])
            self._vertical_true[1].append(sample.velocity_mps[2])
            appended = True
        return appended

    def _drain_telemetry(self) -> bool:
        appended = False
        for datagram in drain(self._telemetry_socket):
            sample = decode_telemetry(datagram)
            if sample is None:
                self._dropped += 1
                continue
            self._received += 1
            self._telemetry_times.append(self._elapsed_s(sample.timestamp_us))
            for axis, rate in enumerate(sample.gyro_rad_s):
                self._gyro[axis].append(rate)
            for axis, angle in enumerate(euler_deg(sample.attitude_quat)):
                self._euler_estimated[axis].append(angle)
            if self._last_true_quat is None:
                self._error_deg.append(0.0)
            else:
                self._error_deg.append(
                    error_angle_deg(sample.attitude_quat, self._last_true_quat)
                )
            self._vertical_estimated[0].append(sample.altitude_m)
            self._vertical_estimated[1].append(sample.vertical_velocity_mps)
            appended = True
        return appended

    def _refresh_plot(self) -> None:
        """Push the buffers to the curves and slide the visible time window."""
        telemetry_times = list(self._telemetry_times)
        for curve, buffer in zip(self._gyro_curves, self._gyro):
            curve.setData(telemetry_times, list(buffer))
        for curve, buffer in zip(self._estimated_curves, self._euler_estimated):
            curve.setData(telemetry_times, list(buffer))
        self._error_curve.setData(telemetry_times, list(self._error_deg))
        for curve, buffer in zip(self._vertical_estimated_curves, self._vertical_estimated):
            curve.setData(telemetry_times, list(buffer))

        sim_raw_times = list(self._sim_raw_times)
        for curve, buffer in zip(self._true_curves, self._euler_true):
            curve.setData(sim_raw_times, list(buffer))
        for curve, buffer in zip(self._vertical_true_curves, self._vertical_true):
            curve.setData(sim_raw_times, list(buffer))

        latest = max(
            telemetry_times[-1] if telemetry_times else 0.0,
            sim_raw_times[-1] if sim_raw_times else 0.0,
        )
        self._plots[0].setXRange(
            max(0.0, latest - self._window_s), max(latest, self._window_s)
        )

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
        "--telemetry-port",
        type=int,
        default=TELEMETRY_PORT,
        help="UDP port of the telemetry broadcast (default: %(default)s)",
    )
    parser.add_argument(
        "--sim-raw-port",
        type=int,
        default=SIM_RAW_PORT,
        help="UDP port of the raw simulator broadcast (default: %(default)s)",
    )
    parser.add_argument(
        "--window",
        type=float,
        default=DEFAULT_WINDOW_S,
        help="seconds of history shown (default: %(default)s)",
    )
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    """Open the sockets, build the window, run the Qt loop."""
    args = parse_arguments(argv)
    if args.window <= 0.0:
        print("--window must be strictly positive", file=sys.stderr)
        return 2

    try:
        telemetry_socket = open_socket(args.telemetry_port)
        sim_raw_socket = open_socket(args.sim_raw_port)
    except OSError as error:
        print("cannot bind UDP port: {}".format(error), file=sys.stderr)
        return 1

    application = QtWidgets.QApplication(sys.argv[:1])
    widget = GroundStationWindow(telemetry_socket, sim_raw_socket, args.window)
    widget.resize(900, 1000)
    widget.show()
    print(
        "listening on 0.0.0.0:{} (telemetry) and 0.0.0.0:{} (sim raw)".format(
            args.telemetry_port, args.sim_raw_port
        )
    )
    return application.exec()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
