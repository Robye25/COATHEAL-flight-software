"""The E-Link hard cap (docs/link-budget.md, "Hard cap"): the kernel shaper
that scripts/link_cap.sh installs, the three places its numbers are written
down, the console's read-out of it, and -- where this machine allows a
throwaway network namespace -- the shaper itself under a flood."""
from __future__ import annotations

import json
import platform
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app import link_budget, link_cap  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "link_cap.sh"
SELFTEST = REPO / "scripts" / "link_cap_selftest.py"
HEADER = REPO / "onboard" / "include" / "coatheal" / "link_budget.hpp"


def shell_constants() -> dict:
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"^([A-Z_]+)=(\d+)\b", SCRIPT.read_text(encoding="utf-8"), re.M)}


def header_constants() -> dict:
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"constexpr std::uint32_t (k\w+) = (\d+);", HEADER.read_text(encoding="utf-8"))}


class OneSetOfNumbersTests(unittest.TestCase):
    """The script shapes, the two ledgers model: all three must say the same."""

    def test_script_onboard_and_ground_station_agree(self) -> None:
        sh, hpp = shell_constants(), header_constants()
        self.assertEqual(sh["ONBOARD_RATE_BYTES"], hpp["kOnboardEgressRateBytesPerS"])
        self.assertEqual(sh["ONBOARD_BUCKET_BYTES"], hpp["kOnboardEgressBurstBytes"])
        self.assertEqual(sh["GROUND_RATE_BYTES"], hpp["kGroundEgressRateBytesPerS"])
        self.assertEqual(sh["GROUND_BUCKET_BYTES"], hpp["kGroundEgressBurstBytes"])
        self.assertEqual(sh["LINK_MTU"], hpp["kLinkMtu"])
        self.assertEqual(sh["WIRE_OVERHEAD"], hpp["kFrameOverhead"])
        self.assertEqual(sh["ONBOARD_RATE_BYTES"], link_budget.ONBOARD_EGRESS_RATE)
        self.assertEqual(sh["ONBOARD_BUCKET_BYTES"], link_budget.ONBOARD_EGRESS_BURST)
        self.assertEqual(sh["GROUND_RATE_BYTES"], link_budget.GROUND_EGRESS_RATE)
        self.assertEqual(sh["GROUND_BUCKET_BYTES"], link_budget.GROUND_EGRESS_BURST)
        self.assertEqual(sh["LINK_MTU"], link_budget.LINK_MTU)
        self.assertEqual(sh["WIRE_OVERHEAD"], link_budget.WIRE_OVERHEAD)
        self.assertEqual(sh["MIN_WIRE_FRAME"], link_budget.MIN_FRAME + link_budget.WIRE_OVERHEAD)
        self.assertEqual(hpp["kEgressModelRatePercent"], link_budget.EGRESS_MODEL_RATE_PERCENT)

    def test_the_two_shapers_together_are_the_cap(self) -> None:
        sh = shell_constants()
        busiest = (sh["ONBOARD_RATE_BYTES"] + sh["ONBOARD_BUCKET_BYTES"]
                   + sh["GROUND_RATE_BYTES"] + sh["GROUND_BUCKET_BYTES"])
        self.assertEqual(busiest, link_budget.CAP_BYTES)
        self.assertEqual(busiest * 8, 24000)
        # A frame at the capped MTU fits either bucket, or it could never pass.
        largest = sh["LINK_MTU"] + 14 + sh["WIRE_OVERHEAD"]
        self.assertLessEqual(largest, sh["GROUND_BUCKET_BYTES"])
        self.assertLessEqual(largest, sh["ONBOARD_BUCKET_BYTES"])

    def test_selftest_limits_are_the_same_numbers(self) -> None:
        text = SELFTEST.read_text(encoding="utf-8")
        self.assertIn("ONBOARD_SECOND, GROUND_SECOND = 1800, 1200", text)
        self.assertEqual(link_budget.ONBOARD_EGRESS_BURST + link_budget.ONBOARD_EGRESS_RATE, 1800)
        self.assertEqual(link_budget.GROUND_EGRESS_BURST + link_budget.GROUND_EGRESS_RATE, 1200)


# What `tc qdisc show` printed on the bench kernel (6.14, iproute2 6.1), 2026-10-05.
TC_GROUND = "qdisc tbf 8006: root refcnt 5 rate 4Kbit burst 700b lat 952ms overhead 24 \n"
TC_ONBOARD = "qdisc tbf 8005: root refcnt 5 rate 6400bit burst 1000b lat 970ms overhead 24 \n"
TC_DETAIL = "qdisc tbf 8002: root refcnt 5 rate 6400bit burst 1Kb/1 mpu 84b lat 370ms overhead 24 linklayer ethernet \n"
TC_NONE = "qdisc noqueue 0: root refcnt 2 \n"
TC_DEFAULT = "qdisc fq_codel 0: root refcnt 2 limit 10240p flows 1024 quantum 1514 target 5ms\n"


class ReadOutTests(unittest.TestCase):
    def test_parse_tbf(self) -> None:
        self.assertEqual(link_cap.parse_tbf(TC_GROUND), (4000, 700))
        self.assertEqual(link_cap.parse_tbf(TC_ONBOARD), (6400, 1000))
        self.assertEqual(link_cap.parse_tbf(TC_DETAIL), (6400, 1024))
        self.assertIsNone(link_cap.parse_tbf(TC_NONE))
        self.assertIsNone(link_cap.parse_tbf(TC_DEFAULT))
        self.assertIsNone(link_cap.parse_tbf(""))

    @staticmethod
    def runner(qdisc: str, mtu: int = 576, route: bool = True):
        def run(cmd: list):
            if cmd[:4] == ["ip", "-o", "route", "get"]:
                return "169.254.10.10 dev enp3s0 src 169.254.10.20 uid 1000 \\    cache \n" if route else None
            if cmd[:3] == ["tc", "qdisc", "show"]:
                return qdisc
            if cmd[:4] == ["ip", "-o", "link", "show"]:
                return f"2: enp3s0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu {mtu} qdisc tbf state UP mode DEFAULT\n"
            return None
        return run

    def test_capped_port_reads_on(self) -> None:
        status = link_cap.ground_cap_status("169.254.10.10", run=self.runner(TC_GROUND), system="Linux")
        self.assertEqual((status.state, status.port, status.ok), ("on", "enp3s0", True))
        self.assertIn("500 B/s", status.detail)
        self.assertIn("1200 B in any second", status.detail)

    def test_anything_else_reads_off_and_names_the_fix(self) -> None:
        for qdisc, mtu in ((TC_NONE, 1500), (TC_DEFAULT, 1500),
                           (TC_ONBOARD, 576),      # the other side's shaper: too fast for this one
                           (TC_GROUND, 1500)):     # shaper there, MTU reset: larger frames can never pass
            status = link_cap.ground_cap_status("169.254.10.10", run=self.runner(qdisc, mtu), system="Linux")
            self.assertEqual((status.state, status.ok), ("off", False), (qdisc, mtu))
            self.assertIn("--link-cap enp3s0", status.detail)

    def test_loopback_other_systems_and_no_route(self) -> None:
        self.assertEqual(link_cap.ground_cap_status("127.0.0.1", system="Linux").state, "n/a")
        self.assertTrue(link_cap.ground_cap_status("127.0.0.1", system="Linux").ok)
        windows = link_cap.ground_cap_status("169.254.10.10", system="Windows")
        self.assertEqual((windows.state, windows.ok), ("unknown", False))
        lost = link_cap.ground_cap_status("169.254.10.10", run=self.runner(TC_GROUND, route=False), system="Linux")
        self.assertEqual(lost.state, "unknown")

    # MUTATION: accept any tbf rate in ground_cap_status and confirm
    # test_anything_else_reads_off_and_names_the_fix fails on the onboard shaper.


def _namespaces_available() -> bool:
    if platform.system() != "Linux" or any(shutil.which(t) is None for t in ("unshare", "nsenter", "ip", "tc")):
        return False
    try:
        return subprocess.run(["unshare", "-Urn", "true"], capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


@unittest.skipUnless(_namespaces_available(), "needs Linux network namespaces (unshare -Urn), ip and tc")
class KernelShaperTests(unittest.TestCase):
    """The real thing: link_cap.sh's shapers on this kernel, flooded from
    both ends inside throwaway namespaces (scripts/link_cap_selftest.py)."""

    def run_selftest(self, *extra: str) -> tuple:
        done = subprocess.run([sys.executable, str(SELFTEST), "--seconds", "8", "--json", *extra],
                              capture_output=True, text=True, timeout=120)
        lines = [line for line in done.stdout.splitlines() if line.startswith("{")]
        return done.returncode, (json.loads(lines[-1]) if lines else {}), done.stdout + done.stderr

    def test_no_second_carries_more_than_24_kbps_under_a_flood_from_both_ends(self) -> None:
        code, report, output = self.run_selftest()
        if code == 3:
            self.skipTest(f"this machine cannot run the shaper test: {output.strip()}")
        self.assertEqual(code, 0, output)
        self.assertEqual(report["result"], "PASS")
        self.assertLessEqual(report["onboard_worst_second"], 1800)
        self.assertLessEqual(report["ground_worst_second"], 1200)
        self.assertLessEqual(report["total_worst_second"], link_budget.CAP_BYTES)
        # The flood did press on both shapers: each passed most of what it may.
        self.assertGreater(report["onboard_worst_second"], 1200)
        self.assertGreater(report["ground_worst_second"], 800)
        self.assertLessEqual(report["largest_frame"], link_budget.LINK_MTU + 14 + 24)

    def test_on_and_off_round_trip_as_the_operator_runs_them(self) -> None:
        # `coatheal-link-cap on` / `off` with a stand-in systemctl that does
        # what the unit does (ExecStart = apply, ExecStop = clear), on a
        # throwaway port in a throwaway namespace.
        script = r'''
set -eu
T="$(mktemp -d)"
export COATHEAL_LINK_CAP_ENV="$T/link-cap.env" COATHEAL_LINK_CAP_STATE="$T/state"
mkdir "$T/bin"
cat > "$T/bin/systemctl" <<STUB
#!/bin/bash
echo "\$*" >> "$T/systemctl.log"
case "\$1" in
  restart) exec bash "$LINK_CAP" apply --role onboard ;;
  disable) exec bash "$LINK_CAP" clear ;;
esac
exit 0
STUB
chmod +x "$T/bin/systemctl"
export PATH="$T/bin:$PATH"
ip link add eth0 type veth peer name capt-t1
ip link add capt-t0 type veth peer name capt-t2
ip link set eth0 up
ip link set capt-t0 up
mtu() { ip -o link show dev "$1" | sed -n '1s/.* mtu \([0-9]*\).*/\1/p'; }
# No port named and none remembered: the default port.
bash "$LINK_CAP" on --role onboard
echo "ENV0=[$(tr '\n' ' ' < "$COATHEAL_LINK_CAP_ENV")]"
echo "MTU0=$(mtu eth0)"
bash "$LINK_CAP" off --role onboard
echo "MTU0OFF=$(mtu eth0)"
bash "$LINK_CAP" on --role onboard --iface capt-t0
echo "ENV1=$(tr '\n' ' ' < "$COATHEAL_LINK_CAP_ENV")"
echo "MTU1=$(mtu capt-t0)"
# No --iface from here on: the port is remembered.
bash "$LINK_CAP" status --role onboard && echo "STATUS1=on"
bash "$LINK_CAP" on --role onboard
echo "ENV2=$(tr '\n' ' ' < "$COATHEAL_LINK_CAP_ENV")"
echo "STATE2=$(cat "$COATHEAL_LINK_CAP_STATE")"
bash "$LINK_CAP" off --role onboard
echo "ENV3=$(tr '\n' ' ' < "$COATHEAL_LINK_CAP_ENV")"
echo "MTU3=$(mtu capt-t0)"
bash "$LINK_CAP" status --role onboard || echo "STATUS3=off"
echo "CALLS=$(cut -d' ' -f1 "$T/systemctl.log" | tr '\n' ' ')"
rm -rf "$T"
'''
        done = subprocess.run(["unshare", "-Urn", "bash", "-c", script], capture_output=True, text=True,
                              timeout=60, env={**__import__("os").environ, "LINK_CAP": str(SCRIPT)})
        out = done.stdout + done.stderr
        self.assertEqual(done.returncode, 0, out)
        self.assertIn("ENV0=[COATHEAL_LINK_CAP=on ]", out, "no port named: the unit picks the default one")
        self.assertIn("MTU0=576", out)
        self.assertIn("MTU0OFF=1500", out)
        self.assertIn("ENV1=COATHEAL_LINK_CAP=on COATHEAL_ELINK_IFACE=capt-t0", out)
        self.assertIn("MTU1=576", out)
        self.assertIn("STATUS1=on", out)
        self.assertIn("ENV2=COATHEAL_LINK_CAP=on COATHEAL_ELINK_IFACE=capt-t0", out, "the port survives a second `on`")
        self.assertIn("STATE2=on capt-t0 800 1000 576 1500 onboard", out,
                      "re-applying keeps the MTU to restore, not the capped one")
        self.assertIn("ENV3=COATHEAL_LINK_CAP=off COATHEAL_ELINK_IFACE=capt-t0", out)
        self.assertIn("MTU3=1500", out)
        self.assertIn("STATUS3=off", out)
        self.assertIn("CALLS=cat enable restart cat disable cat enable restart cat enable restart cat disable", out)
        # MUTATION: write the port line of persist() in scripts/link_cap.sh as
        # `[[ -n "$IFACE" ]] && echo ...` again and confirm the first `on`
        # (no port named) dies under pipefail before it reaches systemctl.

    def test_the_same_flood_without_the_shapers_fails_the_test(self) -> None:
        code, report, output = self.run_selftest("--without-cap", "--seconds", "3")
        if code == 3:
            self.skipTest(f"this machine cannot run the shaper test: {output.strip()}")
        self.assertEqual(code, 1, output)
        self.assertEqual(report["result"], "FAIL")
        self.assertGreater(report["total_worst_second"], link_budget.CAP_BYTES)


if __name__ == "__main__":
    unittest.main()
