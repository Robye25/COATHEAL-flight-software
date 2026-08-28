"""Append-only time series for the plots (redesign spec §5.6).

Each series keeps its own growing numpy arrays (capacity doubles, so an
append is O(1) amortised) because not every series advances on every
frame -- resistance only reports for the monitored specimens, targets only
while set. Windows are sliced with `searchsorted` on the monotonic time
axis. No Qt, no pyqtgraph here.
"""
from __future__ import annotations

from typing import Dict, Iterable, List, Mapping, Optional, Tuple

import numpy as np


class _Series:
    __slots__ = ("t", "y", "n")

    def __init__(self, capacity: int):
        self.t = np.empty(capacity, dtype=np.float64)
        self.y = np.empty(capacity, dtype=np.float64)
        self.n = 0

    def append(self, t: float, y: float) -> None:
        if self.n == self.t.shape[0]:
            new_cap = max(64, self.t.shape[0] * 2)
            t2 = np.empty(new_cap, dtype=np.float64); t2[: self.n] = self.t[: self.n]; self.t = t2
            y2 = np.empty(new_cap, dtype=np.float64); y2[: self.n] = self.y[: self.n]; self.y = y2
        if self.n and t < self.t[self.n - 1]:
            # A backlog frame arriving after live ones (live-first drain):
            # keep the array time-sorted so window slicing stays valid.
            idx = int(np.searchsorted(self.t[: self.n], t, side="right"))
            self.t[idx + 1: self.n + 1] = self.t[idx: self.n]
            self.y[idx + 1: self.n + 1] = self.y[idx: self.n]
            self.t[idx] = t
            self.y[idx] = y
            self.n += 1
            return
        self.t[self.n] = t
        self.y[self.n] = y
        self.n += 1


class SeriesStore:
    def __init__(self, initial_capacity: int = 4096):
        self._cap = max(16, int(initial_capacity))
        self._series: Dict[str, _Series] = {}
        self._t0: Optional[float] = None
        self._t_last: Optional[float] = None
        self._count = 0

    # -- writing -------------------------------------------------------------
    def append(self, t: float, values: Mapping[str, float]) -> None:
        """Append `values` at time `t` (seconds, monotonic non-decreasing).
        NaN values are stored as gaps; None values are skipped."""
        t = float(t)
        if self._t0 is None or t < self._t0:
            self._t0 = t
        if self._t_last is None or t > self._t_last:
            self._t_last = t
        self._count += 1
        for name, value in values.items():
            if value is None:
                continue
            series = self._series.get(name)
            if series is None:
                series = self._series[name] = _Series(self._cap)
            series.append(t, float(value))

    def clear(self) -> None:
        self._series.clear()
        self._t0 = None
        self._t_last = None
        self._count = 0

    # -- reading -------------------------------------------------------------
    @property
    def t0(self) -> Optional[float]:
        return self._t0

    @property
    def t_last(self) -> Optional[float]:
        return self._t_last

    @property
    def count(self) -> int:
        return self._count

    def names(self) -> List[str]:
        return list(self._series)

    def length(self, name: str) -> int:
        series = self._series.get(name)
        return series.n if series else 0

    def series(self, name: str) -> Tuple[np.ndarray, np.ndarray]:
        s = self._series.get(name)
        if s is None:
            return np.empty(0), np.empty(0)
        return s.t[: s.n], s.y[: s.n]

    def window(self, name: str, t_min: Optional[float], t_max: Optional[float] = None
               ) -> Tuple[np.ndarray, np.ndarray]:
        """Points with t_min <= t <= t_max (either bound may be None)."""
        t, y = self.series(name)
        if t.shape[0] == 0:
            return t, y
        lo = 0 if t_min is None else int(np.searchsorted(t, t_min, side="left"))
        hi = t.shape[0] if t_max is None else int(np.searchsorted(t, t_max, side="right"))
        return t[lo:hi], y[lo:hi]

    def latest(self, name: str) -> Optional[float]:
        s = self._series.get(name)
        if s is None or s.n == 0:
            return None
        return float(s.y[s.n - 1])

    def last_before(self, name: str, t: float) -> Optional[float]:
        """Value of the last point at or before `t` (for the crosshair)."""
        ts, ys = self.series(name)
        if ts.shape[0] == 0:
            return None
        idx = int(np.searchsorted(ts, t, side="right")) - 1
        return float(ys[idx]) if idx >= 0 else None

    def nearest(self, name: str, t: float) -> Optional[Tuple[float, float]]:
        ts, ys = self.series(name)
        if ts.shape[0] == 0:
            return None
        idx = int(np.searchsorted(ts, t))
        if idx >= ts.shape[0]:
            idx = ts.shape[0] - 1
        elif idx > 0 and (t - ts[idx - 1]) <= (ts[idx] - t):
            idx -= 1
        return float(ts[idx]), float(ys[idx])


def window_bounds(t_last: Optional[float], span_s: Optional[float]) -> Tuple[Optional[float], Optional[float]]:
    """(t_min, t_max) for a trailing window of `span_s` seconds ending at
    `t_last`; span None means everything."""
    if span_s is None or t_last is None:
        return None, None
    return t_last - float(span_s), None


def format_elapsed(seconds: float) -> str:
    """T+hh:mm:ss (negative values render as -hh:mm:ss)."""
    sign = "-" if seconds < 0 else ""
    seconds = abs(int(round(seconds)))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{sign}{h:02d}:{m:02d}:{s:02d}"
