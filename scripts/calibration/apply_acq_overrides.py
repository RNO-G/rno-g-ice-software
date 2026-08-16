#!/usr/bin/env python3
"""Apply a set of overrides to an rno-g-acq libconfig file and write a copy.

Reads a *template* libconfig file plus a JSON *overrides* file that maps dotted
parameter paths to new values, applies them in place, and writes the result to
an output path.

Key properties (matter for a periodically-run service):
  * Surgical:    only the targeted scalar assignments are touched. All comments,
                 ordering, indentation and the rest of the file are preserved
                 byte-for-byte.
  * Path-scoped: targets are dotted paths (e.g. ``calib.atten``) so that the
                 four different ``atten``/``attenuation`` keys in the file
                 (pedestals, bias_scan, calib, calib.sweep) never collide.
  * Type-coerced: the new value is rendered to match the *existing* literal
                 type in the template (quoted string, float-with-dot, or int;
                 booleans are integers 0/1), so you can't accidentally turn a
                 float field into an int and break the libconfig C reader.
  * All-or-nothing: if any requested path is not found (or fails validation),
                 nothing is written. Safe to fail and retry.
  * Atomic:      output is written to a temp file in the same directory and
                 os.replace()'d into place, so a running DAQ never sees a
                 half-written cfg.
  * Idempotent:  running twice with the same overrides yields the same file.

Usage:
    apply_acq_overrides.py -t acq.cfg.template -o overrides.json \
                           -O /rno-g/cfg/acq.cfg
    apply_acq_overrides.py -t acq.cfg.template -o overrides.json --dry-run
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile

# A section header: "name:" optionally followed by "{" on the same line.
SECTION_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*:\s*(\{)?\s*$")
OPEN_RE = re.compile(r"^\s*\{\s*$")
CLOSE_RE = re.compile(r"^\s*\}\s*;?\s*$")
# A scalar assignment: indent, key, "=", value (up to the first ";"), rest.
# NOTE: assumes the value does not itself contain a ";" -- true for every
# scalar in acq.cfg. Array/group values are detected and refused below.
ASSIGN_RE = re.compile(
    r"^(?P<indent>\s*)(?P<key>[A-Za-z_]\w*)(?P<eq>\s*=\s*)"
    r"(?P<value>.*?)(?P<post>\s*;.*)$"
)

def _is_half_db_step(v: float) -> bool:
    return 0.0 <= v <= 31.5 and round((v / 0.5), 6).is_integer()

STATION_ID_FILE = "/STATION_ID"  # default

VALIDATORS = {
    "calib.enable_cal": lambda v: int(v) in (0, 1),
    "calib.turn_off_at_exit": lambda v: int(v) in (0, 1),
    "calib.channel": lambda v: str(v) in ("none", "coax", "fiber0", "fiber1"),
    "calib.type": lambda v: str(v) in ("none", "pulser", "vco", "vco2"),
    "calib.atten": lambda v: _is_half_db_step(float(v)),
    "calib.sweep.enable": lambda v: int(v) in (0, 1),
    "calib.sweep.start_atten": lambda v: _is_half_db_step(float(v)),
    "calib.sweep.stop_atten": lambda v: _is_half_db_step(float(v)),
    "calib.sweep.atten_step": lambda v: _is_half_db_step(float(v)),
    "output.seconds_per_run": lambda v: float(v) > 50 and float(v) <= 10000,  # somewhat abitrary
    "didaq.trigger.coinc0.enable": lambda v: int(v) in (0, 1),
    "didaq.trigger.coinc1.enable": lambda v: int(v) in (0, 1),
}


def classify(existing: str) -> str:
    """Infer the literal type of an existing value.

    rno-g-acq uses only quoted strings, integers (incl. hex) and floats.
    Booleans are stored as the integers 0/1, so there is no boolean type:
    a JSON ``true``/``false`` is folded into 0/1 by the int renderer.
    """
    e = existing.strip()
    if len(e) >= 2 and e[0] == '"' and e[-1] == '"':
        return "str"
    if e[:1] in ("[", "{", "("):
        return "aggregate"
    if re.fullmatch(r"[+-]?0[xX][0-9a-fA-F]+", e):
        return "int"  # hex int
    if re.fullmatch(r"[+-]?(\d+\.\d*|\.\d+|\d+(\.\d*)?[eE][+-]?\d+)", e):
        return "float"
    return "int"


def render(value, kind: str) -> str:
    """Render a Python value as a libconfig literal of the given kind."""
    if kind == "aggregate":
        raise ValueError("refusing to override an array/group value")
    if kind == "str":
        s = str(value).replace("\\", "\\\\").replace('"', '\\"')
        return f'"{s}"'
    if kind == "float":
        s = repr(float(value))
        if not re.search(r"[.eE]", s):
            s += ".0"
        return s
    if kind == "int":
        # int(True) == 1, int(False) == 0, so JSON booleans land here too.
        return str(int(value))
    raise ValueError(f"unknown kind {kind!r}")


# --- core ------------------------------------------------------------------
def flatten(d, prefix=""):
    """Flatten nested dicts into dotted keys; pass dotted keys through as-is."""
    out = {}
    for k, v in d.items():
        key = f"{prefix}.{k}" if prefix else str(k)
        if isinstance(v, dict):
            out.update(flatten(v, key))
        else:
            out[key] = v
    return out


def apply_overrides(lines, overrides):
    """Return (new_lines, found_paths). Raises on type/validation problems."""
    targets = {tuple(p.split(".")): v for p, v in overrides.items()}
    found, stack, pending, out = set(), [], None, []

    for raw in lines:
        line = raw.rstrip("\n")

        m = SECTION_RE.match(line)
        if m:
            pending = m.group(1)
            if m.group(2):  # "{" on the same line
                stack.append(pending)
                pending = None
            out.append(raw)
            continue
        if OPEN_RE.match(line):
            stack.append(pending if pending is not None else "")
            pending = None
            out.append(raw)
            continue
        if CLOSE_RE.match(line):
            if stack:
                stack.pop()
            pending = None
            out.append(raw)
            continue

        m = ASSIGN_RE.match(line)
        if m:
            path = tuple(stack) + (m.group("key"),)
            if path in targets:
                value = targets[path]
                dotted = ".".join(path)
                kind = classify(m.group("value"))
                validator = VALIDATORS.get(dotted)
                if validator is None or not validator(value):
                    if validator is None:
                        print(f"Validator for {dotted} is missing", file=sys.stderr)

                    raise ValueError(
                        f"{dotted}: value {value!r} fails validation"
                    )
                new_val = render(value, kind)
                newline = (
                    f"{m.group('indent')}{m.group('key')}"
                    f"{m.group('eq')}{new_val}{m.group('post')}"
                )
                out.append(newline + "\n")
                found.add(path)
                continue

        out.append(raw)

    return out, found


def load_doc(path):
    """Load a JSON document as a dict (no flattening)."""
    if path is None:
        return {}

    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        sys.exit("overrides file must contain a mapping at the top level")
    return data


def reserved_keys(doc):
    """Top-level keys that switch the file into per-station mode. Any key
    starting with "defaults" is a defaults block (e.g. "defaults_all",
    "defaults_didaq"), so several of them can coexist.
    """
    return {k for k in doc
            if k in ("station_source", "stations") or k.startswith("defaults")}


def deep_merge(base, over):
    """Recursively merge ``over`` onto ``base`` (over wins at the leaves)."""
    out = dict(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def resolve_station(cli_station, source_cfg):
    """Determine which station we're on. First hit wins:

    1. ``--station`` on the command line (explicit; for testing/manual runs)
    2. contents of the file at ``station_source.file`` (default: /STATION_ID)

    Returns the resolved id as a stripped string.
    """
    if cli_station is not None:
        return str(cli_station).strip()

    src = source_cfg or {}
    path = src.get("file", STATION_ID_FILE)
    if path and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()

    sys.exit("error: could not determine station id — use --station or set station_source.file")


def station_matches(station, key):
    """True if ``key`` (a stations-mapping key or an entry of a ``stations``
    list) refers to ``station``.

    Compared as strings first, then numerically with any leading non-digits
    stripped, so "11", 11, "011" and "station11" all denote the same station.
    """
    station, key = str(station).strip(), str(key).strip()
    if station == key:
        return True
    a, b = re.sub(r"^\D*", "", station), re.sub(r"^\D*", "", key)
    return bool(a) and bool(b) and a.isdigit() and b.isdigit() and int(a) == int(b)


def select_station_block(stations, station):
    """Pick the override block for ``station`` from the ``stations`` mapping."""
    if station in stations:
        return stations[station], station
    for key, block in stations.items():
        if station_matches(station, key):
            return block, key
    return None, None


def collect_defaults(doc, station):
    """Merge every top-level ``defaults*`` block that applies to ``station``.

    A block without a ``stations`` list applies to all stations; with one, only
    to the listed ids. Blocks are merged in document order, so a later block
    wins over an earlier one on a shared leaf. Returns (merged, applied_names).
    """
    merged, applied = {}, []
    for name, block in doc.items():
        if not name.startswith("defaults"):
            continue
        if not isinstance(block, dict):
            sys.exit(f"error: '{name}' must be a mapping")
        only = block.get("stations")
        if only is not None and not any(station_matches(station, s) for s in only):
            continue
        merged = deep_merge(merged, {k: v for k, v in block.items() if k != "stations"})
        applied.append(name)
    return merged, applied


def build_overrides(doc, cli_station):
    """Return (flat_overrides, station_label, has_station_block).

    Two modes:
      * structured: doc has ``station_source`` / ``stations`` / any ``defaults*``
        key. Resolves the station, then deep-merges the applicable defaults
        blocks and finally that station's own block on top.
      * simple: doc is a flat/nested set of overrides applied unconditionally.
    """
    if not reserved_keys(doc):
        return flatten(doc), None, False  # simple mode, no station logic

    stations = doc.get("stations", {}) or {}  # JSON keys are always strings

    station = resolve_station(cli_station, doc.get("station_source"))
    merged, applied = collect_defaults(doc, station)
    block, matched = select_station_block(stations, station)
    if block is not None:
        merged = deep_merge(merged, block)

    label = str(station)
    if matched is not None and matched != station:
        label += f" (matched '{matched}')"
    elif block is None:
        label += " (no station-specific block)"
    label += f" [{', '.join(applied) if applied else 'no defaults'}]"
    return flatten(merged), label, block is not None


def atomic_write(path, lines):
    d = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".acqcfg.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.writelines(lines)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def unified_diff(old, new, path):
    import difflib

    return "".join(
        difflib.unified_diff(
            old, new, fromfile=f"{path} (orig)", tofile=f"{path} (new)"
        )
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-t", "--template", required=True,
                    help="input libconfig file (read-only, never modified)")
    ap.add_argument("-o", "--overrides", required=False,
                    help="overrides file (.json), dotted or nested")
    ap.add_argument("-O", "--output", required=True,
                    help="output path (defaults to --template, i.e. in place)")
    ap.add_argument("-s", "--station", default=None,
                    help="force the station id (overrides auto-detection)")
    ap.add_argument("--require-station", action="store_true",
                    help="fail if the resolved station has no block in 'stations'")
    ap.add_argument("--allow-missing", action="store_true",
                    help="warn (don't fail) if an override path is not found")
    ap.add_argument("--dry-run", action="store_true",
                    help="print a unified diff and write nothing")
    ap.add_argument("--set", metavar="PATH=VALUE", action="append", default=[],
                    help="override a dotted path, e.g. --set calib.atten=10.0 (repeatable, wins over the overrides file)")
    args = ap.parse_args(argv)

    output = args.output

    with open(args.template, "r", encoding="utf-8") as f:
        lines = f.readlines()

    doc = load_doc(args.overrides)
    overrides, station_label, has_station_block = build_overrides(doc, args.station)

    for item in args.set:
        if "=" not in item:
            sys.exit(f"error: --set {item!r} is not in PATH=VALUE form")
        path, _, value = item.partition("=")
        overrides[path.strip()] = value.strip()

    if station_label is not None:
        print(f"station: {station_label}", file=sys.stderr)
        if not has_station_block and args.require_station:
            sys.exit("error: no station-specific override block found "
                     "(--require-station)")

    if not overrides:
        print("no overrides resolved; nothing to do", file=sys.stderr)
        return 0

    try:
        new_lines, found = apply_overrides(lines, overrides)
    except ValueError as exc:
        sys.exit(f"error: {exc} (nothing written)")

    # Check if parameter in overrides are all present in template config
    missing = {".".join(p) for p in
               (tuple(k.split(".")) for k in overrides)} - \
              {".".join(p) for p in found}
    if missing:
        msg = "override path(s) not found in template: " + ", ".join(sorted(missing))
        if args.allow_missing:
            print("warning: " + msg, file=sys.stderr)
        else:
            sys.exit("error: " + msg + " (nothing written)")

    if args.dry_run:
        diff = unified_diff(lines, new_lines, output)
        sys.stdout.write(diff if diff else "(no changes)\n")
        return 0

    if new_lines != lines or output != args.template:
        atomic_write(output, new_lines)

    print(f"wrote {output} ({len(found)} parameter(s) applied)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
