"""Tell replayed backlog frames from live ones (no Qt).

After a link outage the onboard drains its durable queue in order, at many
frames per second, before any live frame arrives. Every one of those frames
carries the state the onboard had *hours ago*; taking the newest as "now"
paints stale modes, stale flags and wrong gating (bench, 2026-08-28: ARM
acknowledged, yet ENABLE stayed greyed because the replay still said
STANDBY). Two signs, either is enough:

* the frame's onboard timestamp lags the ground clock by clearly more
  than the smallest lag ever seen this session (the smallest lag is the
  clock offset between the two machines plus one tick, so no synchronised
  clocks are needed) -- catches a replay that starts after live frames;
* onboard time is advancing much faster than wall time across the last
  few seconds of frames (a drain pushes several onboard-seconds per real
  second; live telemetry advances at 1x whatever the tick rate) -- catches
  the console starting in the middle of a drain, when every frame is newer
  than the one before and the first sign never fires.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Deque, Optional, Tuple

REPLAY_THRESHOLD_S = 30.0
# Frames stamped by the onboard (`TX=<age>`): the drain sends this tick's
# frame first, so a live frame is 0-1 s old and a backlog frame is as old
# as the outage. No clock comparison is needed at all.
TAG_LIVE_MAX_S = 10.0
DEPTH_WINDOW_S = 15.0
RATE_WINDOW_S = 5.0
RATE_RATIO_THRESHOLD = 1.5   # onboard seconds per wall second
RATE_MIN_SPAN_S = 2.0
RATE_MIN_FRAMES = 4


def parse_onboard_timestamp(text: str) -> Optional[float]:
    """`2026-08-28T15:14:23Z` (optionally with fractional seconds) -> epoch."""
    if not text:
        return None
    raw = text.strip()
    if raw.endswith("Z"):
        raw = raw[:-1]
    for fmt in ("%Y-%m-%dT%H:%M:%S.%f", "%Y-%m-%dT%H:%M:%S"):
        try:
            return datetime.strptime(raw, fmt).replace(tzinfo=timezone.utc).timestamp()
        except ValueError:
            continue
    return None


@dataclass(frozen=True)
class ReplayVerdict:
    is_replay: bool
    behind_s: float          # onboard time lag beyond the session's best (0 when live)
    eta_s: Optional[float]   # seconds until the replay catches up, when estimable
    tagged: bool = False     # the onboard stamps frames with their age (live-first drain)
    backlog_frames: Optional[int] = None   # queue depth reported by the last live frame


class ReplayClassifier:
    def __init__(self, threshold_s: float = REPLAY_THRESHOLD_S):
        self.threshold_s = float(threshold_s)
        self._min_lag: Optional[float] = None
        self._recent: Deque[Tuple[float, float]] = deque()   # (rx, onboard_ts)
        self._depths: Deque[Tuple[float, int]] = deque()     # (rx, queue depth) from live frames
        self._backlog_frames: Optional[int] = None

    def reset(self) -> None:
        self._min_lag = None
        self._recent.clear()
        self._depths.clear()
        self._backlog_frames = None

    def classify(self, onboard_ts: Optional[float], rx: float, *,
                 tx_age_s: Optional[float] = None,
                 queue_depth: Optional[int] = None) -> ReplayVerdict:
        if tx_age_s is not None:
            return self._classify_tagged(tx_age_s, rx, queue_depth)
        if onboard_ts is None:
            return ReplayVerdict(False, 0.0, None)
        lag = rx - onboard_ts
        if self._min_lag is None or lag < self._min_lag:
            self._min_lag = lag
        behind = max(0.0, lag - self._min_lag)
        self._recent.append((rx, onboard_ts))
        while self._recent and (rx - self._recent[0][0]) > RATE_WINDOW_S:
            self._recent.popleft()
        ratio = None
        if len(self._recent) >= RATE_MIN_FRAMES:
            rx0, ts0 = self._recent[0]
            d_rx = rx - rx0
            if d_rx >= RATE_MIN_SPAN_S:
                ratio = (onboard_ts - ts0) / d_rx
        fast = ratio is not None and ratio > RATE_RATIO_THRESHOLD
        is_replay = behind > self.threshold_s or fast
        if is_replay and behind <= self.threshold_s:
            # Started mid-drain: no live frame has fixed the clock offset yet,
            # so the lag itself is the best estimate of how far behind we are
            # (exact when the two clocks agree).
            behind = max(0.0, lag)
        eta = None
        if is_replay and ratio is not None:
            catch_up = ratio - 1.0   # backlog seconds cleared per wall second
            if catch_up > 0.05:
                eta = behind / catch_up
        return ReplayVerdict(is_replay, behind, eta)

    def _classify_tagged(self, age: float, rx: float, depth: Optional[int]) -> ReplayVerdict:
        is_replay = age > TAG_LIVE_MAX_S
        if not is_replay and depth is not None:
            self._backlog_frames = int(depth)
            self._depths.append((rx, int(depth)))
            while self._depths and (rx - self._depths[0][0]) > DEPTH_WINDOW_S:
                self._depths.popleft()
        eta = None
        if len(self._depths) >= 2:
            rx0, d0 = self._depths[0]
            rx1, d1 = self._depths[-1]
            if rx1 - rx0 >= 2.0:
                rate = (d0 - d1) / (rx1 - rx0)   # frames cleared per wall second
                if rate > 0.05 and d1 > 0:
                    eta = d1 / rate
        return ReplayVerdict(is_replay, age if is_replay else 0.0, eta,
                             tagged=True, backlog_frames=self._backlog_frames)

    @property
    def clock_offset_s(self) -> Optional[float]:
        """Best estimate of (ground clock - onboard clock), from live frames."""
        return self._min_lag
