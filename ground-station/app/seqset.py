"""Set of received frame sequence numbers, kept as disjoint ranges.

The onboard's live-first drain delivers this tick's frame before the
backlog, so "seq ≤ last seen" no longer means "already received" -- it
would drop the entire backlog as duplicates. A range set stays tiny in
practice (the live run and the chronological backlog are two growing
ranges) and persists as `[[lo, hi], ...]`.
"""
from __future__ import annotations

import bisect
from typing import Any, Iterable, List, Tuple


class SeqSet:
    def __init__(self, ranges: Iterable[Tuple[int, int]] = ()):
        self._lo: List[int] = []
        self._hi: List[int] = []
        for lo, hi in ranges:
            for seq in (int(lo), int(hi)):
                pass
            self._add_range(int(lo), int(hi))

    # -- queries ---------------------------------------------------------------
    def __contains__(self, seq: int) -> bool:
        i = bisect.bisect_right(self._lo, seq) - 1
        return i >= 0 and self._hi[i] >= seq

    def __len__(self) -> int:
        return sum(hi - lo + 1 for lo, hi in zip(self._lo, self._hi))

    @property
    def max(self) -> int:
        return self._hi[-1] if self._hi else -1

    def ranges(self) -> List[Tuple[int, int]]:
        return list(zip(self._lo, self._hi))

    # -- mutation --------------------------------------------------------------
    def add(self, seq: int) -> bool:
        """Insert `seq`; True when it was not already present."""
        seq = int(seq)
        if seq in self:
            return False
        self._add_range(seq, seq)
        return True

    def _add_range(self, lo: int, hi: int) -> None:
        if hi < lo:
            return
        # Merge with every range touching [lo, hi] (adjacency counts).
        i = bisect.bisect_left(self._lo, lo)
        if i > 0 and self._hi[i - 1] >= lo - 1:
            i -= 1
        j = i
        while j < len(self._lo) and self._lo[j] <= hi + 1:
            j += 1
        if i < j:
            lo = min(lo, self._lo[i])
            hi = max(hi, self._hi[j - 1])
        self._lo[i:j] = [lo]
        self._hi[i:j] = [hi]

    # -- persistence -------------------------------------------------------------
    def to_json(self) -> List[List[int]]:
        return [[lo, hi] for lo, hi in zip(self._lo, self._hi)]

    @classmethod
    def from_json(cls, data: Any) -> "SeqSet":
        """Accepts the range form `[[lo, hi], ...]` and the legacy cursor form
        (a single int meaning "everything up to and including it")."""
        if isinstance(data, bool):
            return cls()
        if isinstance(data, (int, float)):
            n = int(data)
            return cls([(0, n)]) if n >= 0 else cls()
        ranges = []
        for item in data or []:
            try:
                lo, hi = int(item[0]), int(item[1])
            except (TypeError, ValueError, IndexError):
                continue
            ranges.append((lo, hi))
        return cls(ranges)
