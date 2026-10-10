"""The console's command list is the onboard's: KNOWN_COMMANDS, the command
reference and the onboard parser's table (onboard/src/command_parser.cpp)
must name exactly the same commands, so the console never offers a command
the onboard does not recognise and never lacks one it does. No Qt."""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
PARSER = HERE.parents[1] / "onboard" / "src" / "command_parser.cpp"

from app.protocol import ALIASES, COMMAND_REFERENCE, KNOWN_COMMANDS, command_spec  # noqa: E402


class CommandReferenceTests(unittest.TestCase):
    def test_reference_covers_every_known_command_once(self) -> None:
        names = [spec.name for spec in COMMAND_REFERENCE]
        self.assertEqual(len(names), len(set(names)), "a command listed twice")
        self.assertEqual(set(names), KNOWN_COMMANDS)
        for spec in COMMAND_REFERENCE:
            self.assertTrue(spec.summary and spec.group and spec.where, spec.name)
            self.assertEqual(spec.name, spec.name.upper())

    @unittest.skipUnless(PARSER.exists(), "onboard sources not beside the ground station")
    def test_console_commands_are_exactly_the_onboard_parsers(self) -> None:
        onboard = set(re.findall(r'\{"([A-Z_]+)",\s*CommandType::k', PARSER.read_text(encoding="utf-8")))
        self.assertGreater(len(onboard), 40)
        self.assertEqual(KNOWN_COMMANDS - onboard, set(), "the console offers commands the onboard does not know")
        self.assertEqual(onboard - KNOWN_COMMANDS, set(), "the onboard knows commands the console does not list")

    def test_aliases_point_at_listed_commands(self) -> None:
        for alias, target in ALIASES.items():
            self.assertIn(alias, KNOWN_COMMANDS)
            self.assertIn(target, KNOWN_COMMANDS)
            self.assertIn(target, command_spec(alias).summary)

    def test_lookup_is_case_insensitive_and_none_for_strangers(self) -> None:
        self.assertEqual(command_spec("time_sync").name, "TIME_SYNC")
        self.assertIn("ground station", command_spec("TIME_SYNC").summary)
        self.assertIsNone(command_spec("NOPE"))
        self.assertIsNone(command_spec(""))


if __name__ == "__main__":
    unittest.main()
