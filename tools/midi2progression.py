#!/usr/bin/env python3
"""Turn MIDI chord clips into MPC .progression files that Chord Stage (and the MPC) can read.

Usage:
    python3 tools/midi2progression.py <file-or-folder>... --out <folder> [--key "A minor"] [--show]

Each MIDI file becomes one .progression with the same name. Chords are the notes that start
together (within a 1/16 note); a chord that just repeats the one before it (rhythmic stabs)
is kept once, so Sloppy / Lock step through the changes, not every hit. The key comes from
--key, or from the file name ("Trap 1 - A minor.mid"), or is guessed from the notes.
Chord names follow the MPC's style (Amin7, C/E). Standard library only.
"""

import argparse
import json
import os
import re
import struct
import sys

SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
LETTER = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
MAJOR = [0, 2, 4, 5, 7, 9, 11]
MINOR = [0, 2, 3, 5, 7, 8, 10]

# Interval sets (above the root) -> MPC-style suffix
QUALITIES = [
    ((4, 7), ""), ((3, 7), "min"), ((3, 6), "dim"), ((4, 8), "aug"),
    ((4, 7, 11), "maj7"), ((4, 7, 10), "7"), ((3, 7, 10), "min7"), ((3, 6, 10), "min7b5"),
    ((3, 6, 9), "dim7"), ((3, 7, 11), "minmaj7"),
    ((2, 7), "sus2"), ((5, 7), "sus4"), ((4, 7, 9), "6"), ((3, 7, 9), "min6"), ((7,), "5"),
    ((4, 10), "7"), ((3, 10), "min7"), ((4, 11), "maj7"),
    ((2, 4, 7, 10), "9"), ((2, 3, 7, 10), "min9"), ((2, 4, 7, 11), "maj9"), ((2, 4, 7), "add9"),
    ((2, 3, 7), "minadd9"), ((4, 8, 10), "aug7"), ((5, 7, 10), "7sus4"), ((2, 3, 7, 11), "minmaj9"),
    ((2, 4, 9), "6/9"), ((2, 3, 9), "min6/9"), ((2, 4, 7, 9), "6/9"), ((2, 3, 7, 9), "min6/9"),
    ((4, 6, 10), "7b5"), ((2, 4, 6, 11), "maj9#11"), ((4, 7, 11, 2), "maj9"),
]
QMAP = {tuple(sorted(iv)): sym for iv, sym in QUALITIES}


def read_midi(path):
    """Returns (ticks_per_beat, [(start_tick, end_tick, note), ...]) for every note in every track."""
    data = open(path, "rb").read()
    if data[:4] != b"MThd":
        raise ValueError("not a MIDI file")
    hlen, fmt, ntracks, division = struct.unpack(">IHHH", data[4:14])
    tpb = division if division < 0x8000 else 480
    pos = 8 + hlen
    notes = []
    for _ in range(ntracks):
        if data[pos:pos + 4] != b"MTrk":
            break
        tlen = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        i, end = pos + 8, pos + 8 + tlen
        pos = end
        t, status, on = 0, 0, {}

        def vlq():
            nonlocal i
            v = 0
            while True:
                b = data[i]; i += 1
                v = (v << 7) | (b & 0x7F)
                if not b & 0x80:
                    return v

        while i < end:
            t += vlq()
            b = data[i]
            if b & 0x80:
                status = b; i += 1
            if status == 0xFF:
                i += 1; ln = vlq(); i += ln
                continue
            if status in (0xF0, 0xF7):
                ln = vlq(); i += ln
                continue
            hi = status & 0xF0
            if hi in (0xC0, 0xD0):
                i += 1
                continue
            d1, d2 = data[i], data[i + 1]; i += 2
            if hi == 0x90 and d2 > 0:
                on.setdefault(d1, []).append(t)
            elif hi == 0x80 or (hi == 0x90 and d2 == 0):
                if on.get(d1):
                    notes.append((on[d1].pop(0), t, d1))
        for n, starts in on.items():
            for s in starts:
                notes.append((s, t, n))
    notes.sort()
    return tpb, notes


def chord_name(notes):
    """MPC-style name for a set of MIDI notes, with /bass when the lowest note is not the root."""
    pcs = sorted(set(n % 12 for n in notes))
    low = min(notes) % 12
    if len(pcs) == 1:
        return SHARP[pcs[0]]
    for root in [low] + [p for p in pcs if p != low]:
        iv = tuple(sorted(set((p - root) % 12 for p in pcs) - {0}))
        if iv in QMAP:
            return SHARP[root] + QMAP[iv] + ("" if root == low else "/" + SHARP[low])
    # Not a known shape (a missing 3rd, a cluster): name it from the bass note's 3rd and 7th
    iv = set((p - low) % 12 for p in pcs)
    sym = "min" if 3 in iv and 4 not in iv else ""
    sym += "maj7" if 11 in iv else "7" if 10 in iv else ""
    if sym in ("", "min") and 2 in iv:
        sym += "add9"
    return SHARP[low] + sym


def key_from_name(name):
    m = re.search(r"\b([A-G])([#b]?)\s*(major|minor)", name, re.I)
    if not m:
        return None
    pc = (LETTER[m.group(1).upper()] + {"#": 1, "b": -1, "": 0}[m.group(2)]) % 12
    return pc, m.group(3).lower()


def guess_key(notes):
    best = None
    for root in range(12):
        for mode, sc in (("major", MAJOR), ("minor", MINOR)):
            fit = sum(1 for n in notes if (n - root) % 12 in sc)
            tonic = sum(1 for n in notes if n % 12 == root)
            score = fit * 10 + tonic
            if best is None or score > best[0]:
                best = (score, root, mode)
    return best[1], best[2]


def known_shape(ns):
    pcs = set(n % 12 for n in ns)
    return any(tuple(sorted(set((p - r) % 12 for p in pcs) - {0})) in QMAP for r in pcs)


def nameable(ns):
    """The chord without melody notes on top: drops up to 2 top notes until the rest has a name."""
    for drop in range(0, 3):
        cand = ns[:len(ns) - drop] if drop else ns
        if len(set(n % 12 for n in cand)) < 3:
            break
        if known_shape(cand):
            return cand
    return ns


def chords_from(tpb, notes):
    """Chords from a clip: notes that start together (within a 1/16 note) form a chord.

    Fills with fewer than 3 different notes are skipped, melody notes on top of a chord are
    peeled off, a chord repeated (by name) is kept once, and a loop played twice is kept once.
    A clip with no chords at all (an arpeggio) gets one chord per bar from its notes.
    """
    tol = tpb // 4
    groups, cur, start = [], [], None
    for s, e, n in notes:
        if start is None or s - start > tol:
            if cur:
                groups.append(cur)
            cur, start = [], s
        cur.append(n)
    if cur:
        groups.append(cur)
    chords = [nameable(sorted(set(g))) for g in groups if len(set(n % 12 for n in g)) >= 3]
    if not chords:   # arpeggio: every note of a bar makes the chord
        bars = {}
        for s, e, n in notes:
            bars.setdefault(s // (tpb * 4), []).append(n)
        chords = [nameable(sorted(set(v))) for k, v in sorted(bars.items()) if len(set(x % 12 for x in v)) >= 3]
    out = []
    for c in chords:
        if out and chord_name(out[-1]) == chord_name(c):
            continue                      # same chord hit again
        out.append(c)
    if len(out) > 1 and chord_name(out[0]) == chord_name(out[-1]):
        out.pop()                         # loop: last chord leads back into the first
    names = [chord_name(c) for c in out]
    for period in range(1, len(out)):     # a loop played twice (or more): keep one pass
        if len(out) % period == 0 and all(names[i] == names[i % period] for i in range(len(out))):
            return out[:period]
    return out


def convert(path, key_arg=None):
    tpb, notes = read_midi(path)
    chords = chords_from(tpb, notes)
    if not chords:
        raise ValueError("no chords found (only single notes)")
    title = os.path.splitext(os.path.basename(path))[0]
    key = key_from_name(key_arg) if key_arg else key_from_name(title)
    # "Trap 1 - A minor" -> "Trap 1": the key is in the file itself (Chord Stage sets Key from it)
    short = re.split(r"\s*-\s*(?=[A-G][#b]?\s*(?:major|minor))", title, flags=re.I)[0].strip()
    if short:
        title = short
    if key is None:
        key = guess_key([n for c in chords for n in c])
    root, mode = key
    prog = {
        "progression": {
            "name": title,
            "rootNote": SHARP[root],
            "scale": "Major" if mode == "major" else "Minor",
            "recordingOctave": 1,
            "chords": [{"name": chord_name(c), "role": "Root" if i == 0 else "Normal", "notes": c}
                       for i, c in enumerate(chords)],
        }
    }
    return title, prog


def safe_name(title):
    # Characters FAT / exFAT SD cards (and the MPC) do not allow in file names
    return re.sub(r'[\\/:*?"<>|]', "-", title).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--key", help='key for every file, e.g. "A minor" (default: from the file name)')
    ap.add_argument("--show", action="store_true", help="print the chords of each file")
    a = ap.parse_args()
    files = []
    for p in a.inputs:
        if os.path.isdir(p):
            for d, _, fs in os.walk(p):
                files += [os.path.join(d, f) for f in fs if f.lower().endswith((".mid", ".midi"))]
        else:
            files.append(p)
    os.makedirs(a.out, exist_ok=True)
    ok = 0
    for f in sorted(files):
        try:
            title, prog = convert(f, a.key)
        except Exception as e:
            print("SKIP %s: %s" % (os.path.basename(f), e))
            continue
        out = os.path.join(a.out, safe_name(title) + ".progression")
        with open(out, "w") as fh:
            json.dump(prog, fh, indent=2)
        ok += 1
        p = prog["progression"]
        line = "%-34s %-3s %-5s %2d chords" % (safe_name(title), p["rootNote"], p["scale"], len(p["chords"]))
        if a.show:
            line += ": " + " ".join(c["name"] for c in p["chords"])
        print(line)
    print("%d of %d converted -> %s" % (ok, len(files), a.out))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
