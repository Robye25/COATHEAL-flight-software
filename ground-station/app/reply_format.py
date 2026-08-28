"""Pure helpers for the onboard's `key=value;key=value;...` reply bodies.

`STATUS`, `COMPONENTS`, `CHECK`, `GET_THERMAL`, `BENDSEQ_STATUS` and most
ACK bodies use this shape; `STATUS` nests one level with braces
(`seq0={motor=0;zeroed=1;...}`). No Qt here so the console renderer and the
dispatcher's silence tracking can be unit-tested without a display.
"""
from __future__ import annotations

from typing import Dict, List, Tuple


def split_kv_body(body: str) -> List[Tuple[str, str]]:
    """Ordered (key, value) pairs. Brace groups are kept intact as the
    value of their key (one level of nesting, which is all the onboard
    emits). Items without '=' become ('', item). Empty input -> []."""
    pairs: List[Tuple[str, str]] = []
    depth = 0
    current: List[str] = []
    items: List[str] = []
    for ch in body.strip():
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth = max(0, depth - 1)
        if ch == ";" and depth == 0:
            items.append("".join(current))
            current = []
        else:
            current.append(ch)
    if current:
        items.append("".join(current))
    for item in items:
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            key, value = item.split("=", 1)
            value = value.strip()
            if value.startswith("{") and value.endswith("}"):
                value = value[1:-1]
            pairs.append((key.strip(), value))
        else:
            pairs.append(("", item))
    return pairs


def parse_kv_body(body: str) -> Dict[str, str]:
    """Dict view of `split_kv_body` (last duplicate wins)."""
    return {key: value for key, value in split_kv_body(body) if key}


def pretty_kv_body(body: str, indent: str = "  ") -> str:
    """Multi-line, aligned rendering for the console. Nested groups are
    expanded on their own indented lines. Bodies that carry no '=' at all
    are returned unchanged."""
    pairs = split_kv_body(body)
    if not pairs or all(key == "" for key, _ in pairs):
        return body.strip()
    width = max(len(key) for key, _ in pairs)
    lines: List[str] = []
    for key, value in pairs:
        if key == "":
            lines.append(f"{indent}{value}")
            continue
        nested = split_kv_body(value) if ";" in value and "=" in value else []
        if nested:
            lines.append(f"{indent}{key}")
            nested_width = max(len(k) for k, _ in nested)
            for nk, nv in nested:
                lines.append(f"{indent}{indent}{nk.ljust(nested_width)}  {nv}")
        else:
            lines.append(f"{indent}{key.ljust(width)}  {value}")
    return "\n".join(lines)
