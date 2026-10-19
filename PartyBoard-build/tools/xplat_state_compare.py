#!/usr/bin/env python3
"""Compare two PartyBoard per-frame state logs and name the first divergence.

Usage:
    python3 tools/xplat_state_compare.py mac-seat0.state windows-seat0.state

Either file may be gzip-compressed (.gz), like the saved Mac reference run in
dev/test-results/2026-10-16-xplat/mac-arm64-reference.

A state log is written by a netplay copy started with
PARTYBOARD_STATE_LOG=<file> (and optionally PARTYBOARD_STATE_FIELDS=<a>-<b>
for every canonical field of frames a..b). Unlike a desync report, the two
logs do not have to come from two copies that played each other: the point
is to run the same scripted game on two computers (a Mac and a Windows PC,
or two builds) and find the first frame where their simulations differ.
See docs/CROSS_PLATFORM_ONLINE.md.

Exit status: 0 identical over the frames both logs cover, 1 they differ,
2 the logs are not comparable (different hash version, protocol, disc or
scripted session).
"""

from __future__ import annotations

import argparse
import gzip
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

SUBSYSTEMS = (
    "META", "RNG", "GAMEWORK", "PLAYERS", "BOARD", "OBJECTS", "PROCESSES",
    "OVERLAY", "ANIMATION", "SEQUENCE", "HEAPS", "SCENE", "INPUT", "TIMERS",
    "AUDIO", "MINIGAME",
)


@dataclass
class StateLog:
    path: str
    header: Dict[str, str] = field(default_factory=dict)
    digests: Dict[int, Dict[str, str]] = field(default_factory=dict)
    order: List[int] = field(default_factory=list)
    fields: Dict[int, List[Tuple[int, str, str, str]]] = field(default_factory=dict)


def _pairs(text: str) -> Dict[str, str]:
    out = {}
    for token in text.split():
        if "=" in token:
            key, _, value = token.partition("=")
            out[key] = value
    return out


def parse(path: str) -> StateLog:
    log = StateLog(path=path)
    current_fields: Optional[int] = None
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\r\n")
            if not line:
                continue
            head, _, rest = line.partition(" ")
            if head == "DIGEST":
                role, _, values = rest.partition(" ")
                pairs = _pairs(values)
                if "frame" not in pairs:
                    continue
                frame = int(pairs["frame"])
                if frame not in log.digests:
                    log.order.append(frame)
                log.digests[frame] = pairs
            elif head == "FIELDS":
                pairs = _pairs(rest)
                current_fields = int(pairs.get("frame", "-1"))
                log.fields[current_fields] = []
            elif head == "FIELD" and current_fields is not None:
                parts = rest.split()
                if len(parts) >= 4:
                    log.fields[current_fields].append(
                        (int(parts[0]), parts[1], parts[2], parts[3]))
            elif head == "build_maths":
                log.header.update({"maths_" + k: v for k, v in _pairs(rest).items()})
            elif head == "session":
                log.header.update({"session_" + k: v for k, v in _pairs(rest).items()})
            elif line.startswith("build_stamp="):
                log.header["build_stamp"] = line[len("build_stamp="):]
            else:
                log.header.update(_pairs(line))
    return log


def incomparable(a: StateLog, b: StateLog) -> List[str]:
    """Settings that must agree before a difference means anything."""
    problems = []
    for key in ("hash_version", "protocol", "disc", "disc_version",
                "session_players", "session_input_delay", "session_full_game",
                "session_rollback", "session_walk_plan", "session_walk_mode",
                "session_walk_notches", "session_max_turns", "session_player"):
        left, right = a.header.get(key), b.header.get(key)
        if key.startswith("disc") and not (a.header.get("disc") and b.header.get("disc")):
            continue  # A log from before the disc was named at the first frame.
        if left != right:
            problems.append(f"{key}: {left!r} vs {right!r}")
    return problems


def first_field_difference(left, right):
    for index in range(min(len(left), len(right))):
        _, sub_a, name_a, value_a = left[index]
        _, sub_b, name_b, value_b = right[index]
        if (sub_a, name_a, value_a) != (sub_b, name_b, value_b):
            return index, sub_a, name_a, value_a, sub_b, name_b, value_b
    if len(left) != len(right):
        index = min(len(left), len(right))
        longer = left if len(left) > len(right) else right
        _, sub, name, _ = longer[index]
        return index, sub, name, f"count {len(left)}", sub, name, f"count {len(right)}"
    return None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("first")
    parser.add_argument("second")
    parser.add_argument("--context-lines", type=int, default=3,
                        help="matching frames to show before the first difference")
    parser.add_argument("--force", action="store_true",
                        help="compare even when the session settings differ")
    args = parser.parse_args(argv)

    a, b = parse(args.first), parse(args.second)
    for label, log in (("A", a), ("B", b)):
        maths = " ".join(f"{k[6:]}={v}" for k, v in log.header.items() if k.startswith("maths_"))
        print(f"{label}: {log.path}")
        print(f"   build {log.header.get('build_version', '?')} stamp {log.header.get('build_stamp', '?')}")
        print(f"   {maths or 'maths not recorded'}")
        if "rounding" in log.header:
            print(f"   float env: rounding={log.header.get('rounding')} "
                  f"flush_to_zero={log.header.get('flush_to_zero')} "
                  f"denormals_are_zero={log.header.get('denormals_are_zero')}")
        span = f"{log.order[0]}..{log.order[-1]}" if log.order else "none"
        print(f"   {len(log.order)} frames ({span}), field dumps for {sorted(log.fields) or 'no frames'}")

    problems = incomparable(a, b)
    if problems:
        print("\nINCOMPARABLE: the two runs did not play the same scripted session.")
        for problem in problems:
            print(f"  {problem}")
        if not args.force:
            return 2

    common = sorted(set(a.digests) & set(b.digests))
    if not common:
        print("\nNo frame appears in both logs.")
        return 2
    previous: List[int] = []
    for frame in common:
        left, right = a.digests[frame], b.digests[frame]
        if left.get("hash") == right.get("hash") and left.get("context") == right.get("context"):
            previous.append(frame)
            continue
        print(f"\nFIRST DIFFERENT FRAME: {frame}")
        if previous:
            print(f"last matching frame: {previous[-1]}")
        else:
            print("no earlier matching frame: the runs differ from the first logged frame")
        print(f"context: A={left.get('context')} B={right.get('context')}")
        print(f"rng: A frand={left.get('frand')} rand8={left.get('rand8')} "
              f"B frand={right.get('frand')} rand8={right.get('rand8')}")
        print(f"counter: A={left.get('counter')} B={right.get('counter')}")
        differing = [name for name in SUBSYSTEMS if left.get(name) != right.get(name)]
        print("different subsystems: " + (", ".join(differing) or "none (only the frame stamp)"))
        for name in SUBSYSTEMS:
            mark = "DIFFERENT" if left.get(name) != right.get(name) else "OK"
            print(f"  {name:<10} A={left.get(name)} B={right.get(name)} {mark}")
        if frame in a.fields and frame in b.fields:
            difference = first_field_difference(a.fields[frame], b.fields[frame])
            if difference is None:
                print("\nEvery logged field matches at this frame although the hashes differ:"
                      " the field dumps were taken from different builds of the exporter.")
            else:
                index, sub_a, name_a, value_a, sub_b, name_b, value_b = difference
                print("\nFIRST DIFFERENT FIELD")
                print(f"  index:  {index}")
                print(f"  A:      {sub_a} {name_a} = 0x{value_a}")
                print(f"  B:      {sub_b} {name_b} = 0x{value_b}")
                if index > 0:
                    _, sub, name, value = a.fields[frame][index - 1]
                    print(f"  (field before it, equal: {sub} {name} = 0x{value})")
        else:
            start = max(0, (previous[-1] if previous else frame) - 1)
            print(f"\nNo field dump for frame {frame} in both logs. Rerun both sides with"
                  f"\n  PARTYBOARD_STATE_FIELDS={start}-{frame}"
                  f"\nto name the first different value (the runs are deterministic).")
        return 1

    only_a = len(a.order) - len(common)
    only_b = len(b.order) - len(common)
    print(f"\nIDENTICAL over {len(common)} common frames ({common[0]}..{common[-1]}).")
    if only_a or only_b:
        print(f"frames only in A: {only_a}, only in B: {only_b} (one run went further or started later)")
    last_a = a.digests[a.order[-1]]
    last_b = b.digests[b.order[-1]]
    print(f"last frame: A {a.order[-1]} context {last_a.get('context')}, "
          f"B {b.order[-1]} context {last_b.get('context')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
