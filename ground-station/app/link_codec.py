"""z1 telemetry compression (docs/link-budget.md, "Telemetry framing").

After the TCP connection opens the onboard sends `HELLO,<session_id>,z1:<crc32>`
-- the CRC-32 (8 lowercase hex digits) of `protocol/telemetry-dictionary-z1.txt`.
A ground station holding a byte-identical dictionary answers `HELLO,z1`, and
the onboard then sends every DATA/EVT line as `Z1,<base64>`: standard base64
of the raw DEFLATE stream (level 9, 15-bit window without zlib header, memory
level 9, preset dictionary = that file) of the exact text line, without its
newline. Any other answer leaves the connection plain. ACK lines stay plain.

When the dictionary file cannot be read the ground station never offers the
codec: every HELLO is answered `HELLO,plain`. No Qt here.
"""
from __future__ import annotations

import base64
import binascii
import re
import zlib
from pathlib import Path
from typing import List, Optional

DICTIONARY_NAME = "telemetry-dictionary-z1.txt"
# An inflated line longer than this is refused (a DATA line is ~1.2 kB).
MAX_DECODED_BYTES = 65536


class CodecError(ValueError):
    """A `Z1,` line that cannot be decoded."""


def _dictionary_path() -> Path:
    """`<repo>/protocol/telemetry-dictionary-z1.txt`, seen from
    `<repo>/ground-station/app/`."""
    here = Path(__file__).resolve()
    try:
        return here.parents[2] / "protocol" / DICTIONARY_NAME
    except IndexError:   # installed somewhere shallow: no repo around it
        return here.parent / DICTIONARY_NAME


def _load_dictionary(path: Path) -> Optional[bytes]:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    return data or None


DICTIONARY_PATH = _dictionary_path()
DICTIONARY: Optional[bytes] = _load_dictionary(DICTIONARY_PATH)
DICTIONARY_CRC: Optional[str] = (f"{zlib.crc32(DICTIONARY) & 0xFFFFFFFF:08x}"
                                 if DICTIONARY is not None else None)

HELLO_Z1 = "HELLO,z1\n"
HELLO_PLAIN = "HELLO,plain\n"


def available() -> bool:
    return DICTIONARY is not None


def _z1_offers(text: str) -> List[str]:
    """The dictionary CRCs a HELLO line offers z1 with (`;`- or `,`-separated
    offers after the session id)."""
    parts = text.split(",", 2)
    offers = parts[2] if len(parts) > 2 else ""
    crcs: List[str] = []
    for offer in re.split(r"[;,]", offers):
        name, _, crc = offer.strip().partition(":")
        if name == "z1":
            crcs.append(crc.strip().lower())
    return crcs


def hello_reply(line: str) -> Optional[str]:
    """The answer to an onboard `HELLO,<session>,<offers>` line, or None when
    `line` is not a HELLO. `z1` is accepted only when its CRC matches our
    dictionary."""
    text = (line or "").strip()
    if not text.startswith("HELLO,"):
        return None
    if DICTIONARY_CRC is not None and DICTIONARY_CRC in _z1_offers(text):
        return HELLO_Z1
    return HELLO_PLAIN


def describe_hello(line: str, reply: str) -> str:
    """One log line for a HELLO and the answer given to it; a z1 offer that
    had to be turned down reads as a warning (the link then carries plain
    1.2 kB frames)."""
    text = line.strip()
    if reply == HELLO_Z1:
        return f"onboard {text} -> HELLO,z1 (compressed telemetry)"
    if not _z1_offers(text):
        return f"onboard {text} -> HELLO,plain"
    why = (f"its z1 dictionary is not ours ({DICTIONARY_CRC})" if DICTIONARY_CRC is not None
           else f"no dictionary at {DICTIONARY_PATH}")
    return f"WARNING: onboard {text} -> HELLO,plain: {why}; telemetry stays uncompressed"


def decode_line(line: str) -> str:
    """The text line a `Z1,<base64>` frame carries; any other line unchanged."""
    if not line.startswith("Z1,"):
        return line
    if DICTIONARY is None:
        raise CodecError("Z1 frame but no telemetry dictionary on this ground station")
    try:
        raw = base64.b64decode(line[3:].strip(), validate=True)
    except (binascii.Error, ValueError) as exc:
        raise CodecError(f"Z1 frame: bad base64 ({exc})") from exc
    inflater = zlib.decompressobj(wbits=-15, zdict=DICTIONARY)
    try:
        data = inflater.decompress(raw, MAX_DECODED_BYTES)
    except zlib.error as exc:
        raise CodecError(f"Z1 frame: inflate failed ({exc})") from exc
    if inflater.unconsumed_tail:
        raise CodecError(f"Z1 frame: inflates past {MAX_DECODED_BYTES} B")
    if not inflater.eof:
        raise CodecError("Z1 frame: truncated DEFLATE stream")
    return data.decode("utf-8", errors="replace")


def encode_line(text: str) -> str:
    """`Z1,<base64>` for one text line, exactly as the onboard encodes it."""
    if DICTIONARY is None:
        raise CodecError("no telemetry dictionary on this ground station")
    deflater = zlib.compressobj(level=9, method=zlib.DEFLATED, wbits=-15, memLevel=9,
                                zdict=DICTIONARY)
    raw = deflater.compress(text.encode("utf-8")) + deflater.flush()
    return "Z1," + base64.b64encode(raw).decode("ascii")
