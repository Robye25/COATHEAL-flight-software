"""Per-session log directories (redesign spec §7)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.session_dir import (  # noqa: E402
    LATEST_POINTER, SessionDirectory, session_dir_name, session_epoch,
)


class SessionDirNameTests(unittest.TestCase):
    def test_epoch_parsed_from_onboard_session_id(self) -> None:
        # coatheal-<epoch>-<pid>: 1787760547 = 2026-08-26T16:09:07Z.
        self.assertEqual(session_epoch("coatheal-1787760547-462807"), 1787760547)
        self.assertEqual(session_dir_name("coatheal-1787760547-462807"),
                         "20260826-160907_coatheal-1787760547-462807")

    # MUTATION: make session_epoch always return None and confirm
    # test_epoch_parsed_from_onboard_session_id fails (the name would carry
    # the wall-clock stamp instead of 20260826-160907).

    def test_ids_without_epoch_use_the_given_clock(self) -> None:
        self.assertIsNone(session_epoch("sess-smoke"))
        self.assertEqual(session_dir_name("sess-smoke", now=0),
                         "19700101-000000_sess-smoke")

    def test_unsafe_characters_are_neutralised(self) -> None:
        name = session_dir_name("bad/id with spaces", now=0)
        self.assertEqual(name, "19700101-000000_bad_id_with_spaces")


class SessionDirectoryTests(unittest.TestCase):
    def test_ensure_creates_directory_and_latest_pointer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sd = SessionDirectory(root, "coatheal-1787760547-1")
            self.assertFalse(sd.exists())
            path = sd.ensure()
            self.assertTrue(path.is_dir())
            self.assertEqual(path.parent.name, "sessions")
            pointer = (root / LATEST_POINTER).read_text(encoding="utf-8").strip()
            self.assertEqual(pointer, str(path))
            # Same id again resolves to the same directory (GS restart).
            self.assertEqual(SessionDirectory(root, "coatheal-1787760547-1").path, path)

    def test_no_session_directory_is_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sd = SessionDirectory.no_session(Path(tmp), now=0)
            self.assertEqual(sd.session_id, "")
            self.assertTrue(sd.path.name.endswith("_no-session"))


if __name__ == "__main__":
    unittest.main()
