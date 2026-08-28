"""SeriesStore: append/window/nearest and the elapsed-time formatter."""
from __future__ import annotations

import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.gui.series_store import SeriesStore, format_elapsed, window_bounds  # noqa: E402


class SeriesStoreTests(unittest.TestCase):
    def test_append_and_window_boundaries(self) -> None:
        store = SeriesStore(initial_capacity=4)
        for i in range(10):
            store.append(float(i), {"a": i * 2.0, "b": None if i % 2 else float(i)})
        t, y = store.series("a")
        self.assertEqual(list(t), [float(i) for i in range(10)])
        self.assertEqual(store.length("b"), 5, "None values are skipped, not stored")
        t, y = store.window("a", 3.0, 6.0)
        self.assertEqual(list(t), [3.0, 4.0, 5.0, 6.0], "both bounds inclusive")
        t, y = store.window("a", 8.5, None)
        self.assertEqual(list(y), [18.0])
        self.assertEqual(store.latest("a"), 18.0)
        self.assertIsNone(store.latest("zzz"))
        self.assertEqual(store.last_before("a", 4.5), 8.0)
        self.assertEqual(store.nearest("a", 4.4), (4.0, 8.0))
        self.assertEqual(store.nearest("a", 4.6), (5.0, 10.0))
        self.assertEqual(store.t0, 0.0)
        self.assertEqual(store.t_last, 9.0)
        self.assertEqual(store.count, 10)

    # MUTATION: change `side="right"` to `side="left"` for the upper bound
    # in SeriesStore.window and confirm the inclusive-bounds assertion fails
    # (6.0 disappears).

    def test_capacity_growth_is_cheap(self) -> None:
        store = SeriesStore(initial_capacity=16)
        start = time.perf_counter()
        for i in range(100_000):
            store.append(float(i), {"x": 1.0, "y": 2.0})
        elapsed = time.perf_counter() - start
        self.assertEqual(store.length("x"), 100_000)
        self.assertLess(elapsed, 5.0, f"100k appends took {elapsed:.2f}s -- growth is not amortised")

    def test_clear(self) -> None:
        store = SeriesStore()
        store.append(1.0, {"a": 1.0})
        store.clear()
        self.assertEqual(store.names(), [])
        self.assertIsNone(store.t0)


class HelpersTests(unittest.TestCase):
    def test_window_bounds(self) -> None:
        self.assertEqual(window_bounds(100.0, 30.0), (70.0, None))
        self.assertEqual(window_bounds(100.0, None), (None, None))
        self.assertEqual(window_bounds(None, 30.0), (None, None))

    def test_format_elapsed(self) -> None:
        self.assertEqual(format_elapsed(0), "00:00:00")
        self.assertEqual(format_elapsed(3725.4), "01:02:05")
        self.assertEqual(format_elapsed(-61), "-00:01:01")


class OutOfOrderInsertTests(unittest.TestCase):
    """Live-first drains deliver a backlog frame after newer live ones."""

    def test_backlog_points_are_inserted_in_time_order(self) -> None:
        store = SeriesStore()
        store.append(100.0, {"a": 1.0})
        store.append(50.0, {"a": 0.5})
        store.append(75.0, {"a": 0.75})
        store.append(101.0, {"a": 1.01})
        t, y = store.series("a")
        self.assertEqual(list(t), [50.0, 75.0, 100.0, 101.0])
        self.assertEqual(list(y), [0.5, 0.75, 1.0, 1.01])
        self.assertEqual(store.t0, 50.0)
        self.assertEqual(store.t_last, 101.0)
        wt, _wy = store.window("a", 60.0, 100.5)
        self.assertEqual(list(wt), [75.0, 100.0])
        self.assertEqual(store.latest("a"), 1.01)

    # MUTATION: drop the `t < self.t[self.n - 1]` branch in _Series.append and
    # confirm test_backlog_points_are_inserted_in_time_order fails on the
    # sorted-time assertion.


if __name__ == "__main__":
    unittest.main()
