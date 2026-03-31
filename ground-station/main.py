from __future__ import annotations

import argparse
import sys

from app import command_client, telemetry_server


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="COATHEAL ground station tooling")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    telemetry_server.add_subparser(subparsers)
    command_client.add_subparser(subparsers)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    handler = getattr(args, "_coatheal_handler", None)
    if handler is None:
        parser.print_help()
        return 1
    return int(handler(args))


if __name__ == "__main__":
    sys.exit(main())
