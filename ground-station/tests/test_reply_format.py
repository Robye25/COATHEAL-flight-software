"""Pure `key=value;` reply-body helpers (console rendering + silence tracking)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.reply_format import parse_kv_body, pretty_kv_body, split_kv_body  # noqa: E402

STATUS_BODY = (
    "phase=ASCENT;mode=RUN;tick_hz=1;silence=1;"
    "seq0={motor=0;zeroed=1;running=0;paused=0;name=;step=0};"
    "seq1={motor=1;zeroed=0;running=1;paused=1;name=flex;step=2;fault=operator pause}"
)


class SplitTests(unittest.TestCase):
    def test_nested_groups_stay_whole(self) -> None:
        pairs = split_kv_body(STATUS_BODY)
        self.assertEqual(pairs[0], ("phase", "ASCENT"))
        self.assertEqual(pairs[3], ("silence", "1"))
        self.assertEqual(pairs[4][0], "seq0")
        self.assertEqual(pairs[4][1], "motor=0;zeroed=1;running=0;paused=0;name=;step=0")
        self.assertEqual(len(pairs), 6, "the ';' inside braces must not split")

    # MUTATION: remove the depth tracking in split_kv_body (treat every ';'
    # as a separator) and confirm test_nested_groups_stay_whole fails on the
    # pair count (13 instead of 6).

    def test_empty_values_and_bare_items(self) -> None:
        self.assertEqual(split_kv_body("name=;x"), [("name", ""), ("", "x")])
        self.assertEqual(split_kv_body(""), [])
        self.assertEqual(parse_kv_body("a=1;a=2"), {"a": "2"})

    def test_parse_gives_silence_flag(self) -> None:
        self.assertEqual(parse_kv_body(STATUS_BODY)["silence"], "1")
        self.assertNotIn("silence", parse_kv_body("phase=ASCENT;mode=RUN"))


class PrettyTests(unittest.TestCase):
    def test_pretty_aligns_and_expands_groups(self) -> None:
        text = pretty_kv_body(STATUS_BODY, indent="  ")
        lines = text.splitlines()
        self.assertEqual(lines[0], "  phase    ASCENT")
        self.assertIn("  seq1", lines)
        self.assertIn("    fault    operator pause", lines)

    def test_plain_bodies_pass_through(self) -> None:
        self.assertEqual(pretty_kv_body("pong"), "pong")
        self.assertEqual(pretty_kv_body("radio silence active"), "radio silence active")


if __name__ == "__main__":
    unittest.main()
