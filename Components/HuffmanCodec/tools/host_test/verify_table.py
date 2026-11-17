#!/usr/bin/env python3
"""
verify_table.py — Independent verification of a generated huffman_table.h.

Parses the C header (rather than trusting the generator's in-memory state),
checks internal consistency of the canonical Huffman tables, then encodes a
corpus with the firmware's exact tokeniser/encoder algorithm and verifies:

  1. encode(msg) → decode → msg round-trips for every message
  2. (optional) the frames are bit-identical to those produced by the real
     C++ implementation (host_test/test_roundtrip --dump output)

Usage:
    verify_table.py --table ../include/huffman_table.h --corpus corpus.txt \
                    [--frames frames.txt]

Corpus format: one message per line, literal "\\x1b" for ESC — the format
written by huffman_analysis.py --save-corpus.
"""

import argparse
import re
import sys


# ── Header parsing ──────────────────────────────────────────────────────────────

def c_unescape(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\":
            nxt = s[i + 1]
            if nxt == "x":
                out.append(chr(int(s[i + 2:i + 4], 16)))
                i += 4
            else:
                out.append({"\\": "\\", '"': '"', "n": "\n", "t": "\t", "r": "\r", "0": "\0"}[nxt])
                i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def extract_block(text, name):
    m = re.search(r"static const \w+ " + re.escape(name) + r"\[.*?=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit(f"error: array {name} not found in header")
    return m.group(1)


def parse_table(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    defines = dict(re.findall(r"#define\s+(\w+)\s+(\d+)", text))
    ngram_count = int(defines["NGRAM_COUNT"])
    max_bits = int(defines["HUFF_MAX_CODE_BITS"])
    alphabet = int(defines["HUFFMAN_ALPHABET_SIZE"])

    ngrams = [c_unescape(m) for m in re.findall(r'"((?:[^"\\]|\\.)*)"', extract_block(text, "NGRAM_TABLE"))]
    ngram_len = [int(v) for v in re.findall(r"\d+", extract_block(text, "NGRAM_LEN"))]
    ngram_first = [int(v) for v in re.findall(r"\d+", extract_block(text, "NGRAM_FIRST"))]
    ngram_last = [int(v) for v in re.findall(r"\d+", extract_block(text, "NGRAM_LAST"))]
    huff = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{8}", extract_block(text, "HUFFMAN_TABLE"))]
    count = [int(v) for v in re.findall(r"\d+", extract_block(text, "HUFF_DECODE_COUNT"))]
    first = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{8}", extract_block(text, "HUFF_DECODE_FIRST"))]
    group = [int(v) for v in re.findall(r"\d+", extract_block(text, "HUFF_DECODE_GROUP_OFFSET"))]
    symbols = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", extract_block(text, "HUFF_DECODE_SYMBOLS"))]

    assert len(ngrams) == ngram_count, "NGRAM_TABLE size mismatch"
    assert len(ngram_len) == ngram_count, "NGRAM_LEN size mismatch"
    assert len(ngram_first) == 128 and len(ngram_last) == 128
    assert len(huff) == alphabet, "HUFFMAN_TABLE size mismatch"
    assert len(count) == max_bits + 1 and len(first) == max_bits + 1 and len(group) == max_bits + 1

    return {
        "ngrams": ngrams, "ngram_len": ngram_len,
        "ngram_first": ngram_first, "ngram_last": ngram_last,
        "huff": huff, "count": count, "first": first,
        "group": group, "symbols": symbols, "max_bits": max_bits,
    }


# ── Consistency checks ──────────────────────────────────────────────────────────

def check_consistency(t):
    errors = []

    for i, g in enumerate(t["ngrams"]):
        if len(g) != t["ngram_len"][i]:
            errors.append(f"NGRAM_LEN[{i}] = {t['ngram_len'][i]} but string has {len(g)} chars")
    if t["ngrams"] != sorted(t["ngrams"]):
        errors.append("NGRAM_TABLE is not sorted lexicographically")

    for c in range(128):
        lo, hi = t["ngram_first"][c], t["ngram_last"][c]
        idxs = [i for i, g in enumerate(t["ngrams"]) if ord(g[0]) == c]
        if idxs:
            if lo != idxs[0] or hi != idxs[-1] + 1:
                errors.append(f"NGRAM_FIRST/LAST[{c:#04x}] = [{lo},{hi}) but expected [{idxs[0]},{idxs[-1] + 1})")
        elif lo != 255 or hi != 255:
            errors.append(f"NGRAM_FIRST/LAST[{c:#04x}] should be 255 for char without n-grams")

    # Rebuild canonical codes from the decode tables and compare with HUFFMAN_TABLE
    rebuilt = {}
    for L in range(1, t["max_bits"] + 1):
        for k in range(t["count"][L]):
            sym = t["symbols"][t["group"][L] + k]
            rebuilt[sym] = (L << 27) | (t["first"][L] + k)
    for sym in range(len(t["huff"])):
        want = rebuilt.get(sym, 0)
        if t["huff"][sym] != want:
            errors.append(f"HUFFMAN_TABLE[{sym:#04x}] = {t['huff'][sym]:#010x}, decode tables imply {want:#010x}")

    # Kraft inequality — a valid prefix code must satisfy sum(2^-len) <= 1
    kraft = sum(c * 2 ** -L for L, c in enumerate(t["count"]) if L)
    if kraft > 1.0:
        errors.append(f"Kraft inequality violated: {kraft}")

    # Every printable char and every n-gram token must have a code
    for sym in list(range(0x20, 0x7F)) + [0x80 | i for i in range(len(t["ngrams"]))] + [0x00]:
        if (t["huff"][sym] >> 27) == 0:
            errors.append(f"symbol {sym:#04x} has no code but is encodable")

    return errors


# ── Firmware-exact encode/decode ────────────────────────────────────────────────

def encode(msg, t):
    """Mirror HuffmanEncoder::flush — including NGRAM_FIRST/LAST candidate scan."""
    bits = []
    i, n = 0, len(msg)
    while i < n:
        c = ord(msg[i])
        sym, advance = c, 1
        if 0x20 <= c <= 0x7E:
            lo, hi = t["ngram_first"][c], t["ngram_last"][c]
            if lo != 255:
                best_idx, best_len = None, 0
                for j in range(lo, hi):
                    nlen = t["ngram_len"][j]
                    if i + nlen > n or nlen <= best_len:
                        continue
                    if msg.startswith(t["ngrams"][j], i):
                        best_idx, best_len = j, nlen
                if best_idx is not None:
                    sym, advance = 0x80 | best_idx, best_len
        entry = t["huff"][sym]
        bits.append((entry & 0x07FFFFFF, entry >> 27))
        i += advance
    entry = t["huff"][0x00]
    bits.append((entry & 0x07FFFFFF, entry >> 27))

    out = bytearray()
    acc, accn = 0, 0
    for code, length in bits:
        acc = (acc << length) | code
        accn += length
        while accn >= 8:
            out.append((acc >> (accn - 8)) & 0xFF)
            accn -= 8
    if accn:
        out.append((acc << (8 - accn)) & 0xFF)
    return bytes(out)


def decode(frame, t):
    """Mirror HuffmanDecoder::decodeFrame."""
    out = []
    code, L = 0, 0
    for b in frame:
        for shift in range(7, -1, -1):
            code = (code << 1) | ((b >> shift) & 1)
            L += 1
            if L > t["max_bits"]:
                raise ValueError("decode error: exceeded max code length")
            if t["count"][L] and t["first"][L] <= code < t["first"][L] + t["count"][L]:
                sym = t["symbols"][t["group"][L] + code - t["first"][L]]
                if sym == 0x00:
                    return "".join(out)
                out.append(t["ngrams"][sym - 0x80] if sym >= 0x80 else chr(sym))
                code, L = 0, 0
    raise ValueError("decode error: missing EOM")


# ── Main ────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", required=True, help="huffman_table.h to verify")
    ap.add_argument("--corpus", required=True, help="messages, one per line (\\x1b = ESC)")
    ap.add_argument("--frames", help="hex frames from test_roundtrip --dump to compare against")
    ap.add_argument("--max-input", type=int, default=504,
                    help="max round-trippable message length "
                         "(DECODE_BUF_SIZE - NGRAM_MAX_LEN, default: 504)")
    args = ap.parse_args()

    t = parse_table(args.table)
    print(f"Parsed {args.table}: {len(t['ngrams'])} n-grams, max depth {t['max_bits']} bits")

    errors = check_consistency(t)
    if errors:
        for e in errors:
            print("CONSISTENCY:", e, file=sys.stderr)
        sys.exit(f"FAILED: {len(errors)} consistency errors")
    print("Consistency checks OK (sizes, sort order, index tables, canonical codes, Kraft, coverage)")

    with open(args.corpus, "r", encoding="utf-8") as f:
        msgs = [line.rstrip("\n").replace("\\x1b", "\x1b")[:args.max_input] for line in f]
    msgs = [m for m in msgs if m]

    cframes = None
    if args.frames:
        with open(args.frames, "r", encoding="utf-8") as f:
            cframes = [line.strip() for line in f]
        cframes = [x for x in cframes if x]
        if len(cframes) != len(msgs):
            sys.exit(f"FAILED: {len(cframes)} frames but {len(msgs)} corpus messages")

    mismatches = 0
    for i, msg in enumerate(msgs):
        frame = encode(msg, t)
        if decode(frame, t) != msg:
            print(f"ROUND-TRIP mismatch at line {i + 1}: {msg!r}", file=sys.stderr)
            mismatches += 1
        if cframes is not None and frame.hex() != cframes[i]:
            print(f"FRAME mismatch at line {i + 1}:\n  py: {frame.hex()}\n  c : {cframes[i]}", file=sys.stderr)
            mismatches += 1

    if mismatches:
        sys.exit(f"FAILED: {mismatches} mismatches over {len(msgs)} messages")
    extra = " and match the C++ frames bit-for-bit" if cframes is not None else ""
    print(f"OK: {len(msgs):,} messages round-trip{extra}")


if __name__ == "__main__":
    main()
