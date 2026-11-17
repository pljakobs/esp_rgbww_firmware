#!/usr/bin/env python3
"""
huffman_analysis.py — Generate huffman_table.h for the HuffmanCodec component.

Fetches a log corpus from a lightinator-log-service instance, selects the most
valuable n-grams, builds a canonical Huffman code over a 255-symbol alphabet
(0x00 EOM, 0x01-0x7E ASCII, 0x7F unused, 0x80-0xFE n-gram tokens) and emits
the PROGMEM encode/decode tables consumed by HuffmanEncoder/HuffmanDecoder.

Typical use:
    python3 huffman_analysis.py --emit --out ../include/huffman_table.h

N-gram selection is on by default (--ngrams is accepted for compatibility);
pass --no-ngrams for a chars-only code.

The log service URL defaults to $LIGHTINATOR_LOG_SERVER, falling back to
http://192.168.29.5:4821 — override with --server.

A corpus can be cached locally (--save-corpus) and reused (--corpus) to avoid
re-fetching while experimenting with n-gram parameters.

The script always runs a full encode/decode round-trip self-test over the
corpus before emitting anything.
"""

import argparse
import heapq
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from datetime import datetime, timezone

DEFAULT_SERVER = os.environ.get("LIGHTINATOR_LOG_SERVER", "http://192.168.29.5:4821")

EOM = 0x00  # end-of-message symbol
MAX_CODE_FIELD_BITS = 27  # HUFFMAN_TABLE packs the code into bits [26:0]

CONTROL_NAMES = [
    "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
    "BS", "HT", "LF", "VT", "FF", "CR", "SO", "SI",
    "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
    "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US",
]


# ── Corpus acquisition ──────────────────────────────────────────────────────────

def http_get_json(url, timeout=30):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def fetch_controllers(server):
    data = http_get_json(f"{server}/api/v1/controllers")
    return [c["ip"] for c in data.get("items", []) if c.get("ip")]


def fetch_messages(server, ip, max_messages):
    """Page backwards through /api/v1/logs for one controller."""
    messages = []
    before = None
    while len(messages) < max_messages:
        limit = min(2000, max_messages - len(messages))
        params = {"ip": ip, "limit": limit}
        if before:
            params["before"] = before
        url = f"{server}/api/v1/logs?" + urllib.parse.urlencode(params)
        data = http_get_json(url)
        items = data.get("items", [])
        if not items:
            break
        # items are chronological; prepend so the corpus stays oldest-first
        messages = [it.get("message", "") for it in items] + messages
        before = data.get("nextBefore")
        if not before:
            break
    return messages


def sanitize(msg):
    """Keep only bytes the firmware alphabet can encode (0x01-0x7E)."""
    return "".join(ch for ch in msg if 0x01 <= ord(ch) <= 0x7E)


def load_corpus(args):
    if args.corpus:
        with open(args.corpus, "r", encoding="utf-8") as f:
            msgs = [line.rstrip("\n").replace("\\x1b", "\x1b") for line in f]
        msgs = [m for m in (sanitize(m) for m in msgs) if m]
        print(f"Loaded {len(msgs):,} messages from {args.corpus}")
        return msgs, [args.corpus]

    server = args.server.rstrip("/")
    ips = [s.strip() for s in args.ips.split(",") if s.strip()] if args.ips else fetch_controllers(server)
    if not ips:
        sys.exit(f"error: no controllers reported by {server}")

    msgs = []
    sources = []
    for ip in ips:
        try:
            m = fetch_messages(server, ip, args.max_messages)
        except (urllib.error.URLError, OSError) as e:
            print(f"  {ip}: fetch failed ({e}), skipping", file=sys.stderr)
            continue
        m = [s for s in (sanitize(x) for x in m) if s]
        if m:
            sources.append(ip)
            msgs.extend(m)
        print(f"  {ip}: {len(m):,} messages")
    if not msgs:
        sys.exit("error: empty corpus")

    if args.save_corpus:
        with open(args.save_corpus, "w", encoding="utf-8") as f:
            for m in msgs:
                f.write(m.replace("\x1b", "\\x1b") + "\n")
        print(f"Corpus saved to {args.save_corpus}")
    return msgs, sources


# ── N-gram selection ────────────────────────────────────────────────────────────

def printable_runs(msg):
    """Yield maximal runs of printable ASCII (0x20-0x7E) within a message."""
    run_start = None
    for i, ch in enumerate(msg):
        if 0x20 <= ord(ch) <= 0x7E:
            if run_start is None:
                run_start = i
        elif run_start is not None:
            yield msg[run_start:i]
            run_start = None
    if run_start is not None:
        yield msg[run_start:]


def select_ngrams(msgs, count, max_len, keep_per_len=4000):
    """Pick the `count` substrings (len 2..max_len) with the best freq*(len-1) score."""
    candidates = Counter()
    for n in range(2, max_len + 1):
        c = Counter()
        for msg in msgs:
            for run in printable_runs(msg):
                for i in range(len(run) - n + 1):
                    c[run[i:i + n]] += 1
        for s, f in c.most_common(keep_per_len):
            if f > 1:
                candidates[s] = f
        print(f"  n={n}: {len(c):,} distinct, kept top {min(keep_per_len, len(c)):,}")

    scored = sorted(candidates.items(), key=lambda kv: (-(kv[1] * (len(kv[0]) - 1)), kv[0]))
    chosen = [s for s, _ in scored[:count]]
    return sorted(chosen)  # lexicographic, as required by NGRAM_FIRST/LAST


# ── Tokenisation (mirrors HuffmanEncoder::flush) ────────────────────────────────

def build_matcher(ngrams):
    """first char → list of (ngram, token) — encoder picks the longest match."""
    index = {}
    for i, g in enumerate(ngrams):
        index.setdefault(g[0], []).append((g, 0x80 | i))
    for lst in index.values():
        lst.sort(key=lambda e: -len(e[0]))
    return index


def tokenize(msg, matcher):
    syms = []
    i = 0
    n = len(msg)
    while i < n:
        c = msg[i]
        if 0x20 <= ord(c) <= 0x7E:
            for g, tok in matcher.get(c, ()):
                if msg.startswith(g, i):
                    syms.append(tok)
                    i += len(g)
                    break
            else:
                syms.append(ord(c))
                i += 1
        else:
            syms.append(ord(c))
            i += 1
    syms.append(EOM)
    return syms


def symbol_frequencies(msgs, matcher, ngram_count):
    freq = Counter()
    for msg in msgs:
        freq.update(tokenize(msg, matcher))
    # Every printable ASCII char and every selected n-gram must keep a code even
    # if absent from the corpus tokenisation — the firmware encoder may still
    # pick them on future input, and a zero-length code would drop data.
    for c in range(0x20, 0x7F):
        if freq[c] == 0:
            freq[c] = 1
    for i in range(ngram_count):
        if freq[0x80 | i] == 0:
            freq[0x80 | i] = 1
    return freq


# ── Canonical Huffman ───────────────────────────────────────────────────────────

def code_lengths(freq, max_bits=MAX_CODE_FIELD_BITS):
    """Huffman code lengths; frequencies are flattened until depth fits max_bits."""
    f = dict(freq)
    while True:
        lengths = _huffman_lengths(f)
        if max(lengths.values()) <= max_bits:
            return lengths
        f = {s: (v + 1) // 2 for s, v in f.items()}


def _huffman_lengths(freq):
    heap = [(f, sym, sym) for sym, f in freq.items()]  # (freq, tiebreak, node)
    heapq.heapify(heap)
    parent = {}
    counter = 0x10000  # internal node ids above any symbol value
    while len(heap) > 1:
        f1, _, n1 = heapq.heappop(heap)
        f2, _, n2 = heapq.heappop(heap)
        parent[n1] = counter
        parent[n2] = counter
        heapq.heappush(heap, (f1 + f2, counter, counter))
        counter += 1
    lengths = {}
    for sym in freq:
        d, node = 0, sym
        while node in parent:
            node = parent[node]
            d += 1
        lengths[sym] = max(d, 1)
    return lengths


def canonical_codes(lengths):
    """Canonical assignment: symbols sorted by (length, value)."""
    max_bits = max(lengths.values())
    count = [0] * (max_bits + 1)
    for l in lengths.values():
        count[l] += 1
    first = [0] * (max_bits + 1)
    code = 0
    for l in range(1, max_bits + 1):
        code = (code + count[l - 1]) << 1
        first[l] = code
    codes = {}
    next_code = list(first)
    for sym in sorted(lengths, key=lambda s: (lengths[s], s)):
        l = lengths[sym]
        codes[sym] = next_code[l]
        next_code[l] += 1
    return codes, count, first, max_bits


def decode_tables(lengths, count, first, max_bits):
    group_offset = [0] * (max_bits + 1)
    off = 0
    for l in range(1, max_bits + 1):
        group_offset[l] = off
        off += count[l]
    symbols = sorted(lengths, key=lambda s: (lengths[s], s))
    return group_offset, symbols


# ── Round-trip self-test ────────────────────────────────────────────────────────

def encode_bits(syms, lengths, codes):
    bits = 0
    out = []
    acc, accn = 0, 0
    for s in syms:
        l = lengths[s]
        acc = (acc << l) | codes[s]
        accn += l
        bits += l
        while accn >= 8:
            out.append((acc >> (accn - 8)) & 0xFF)
            accn -= 8
    if accn:
        out.append((acc << (8 - accn)) & 0xFF)
    return bytes(out), bits


def decode_stream(data, count, first, group_offset, symbols, max_bits, ngrams):
    out = []
    code, l = 0, 0
    for byte_i in range(len(data)):
        b = data[byte_i]
        for bit in range(7, -1, -1):
            code = (code << 1) | ((b >> bit) & 1)
            l += 1
            if l <= max_bits and count[l] and first[l] <= code < first[l] + count[l]:
                sym = symbols[group_offset[l] + code - first[l]]
                if sym == EOM:
                    return "".join(out)
                out.append(ngrams[sym - 0x80] if sym >= 0x80 else chr(sym))
                code, l = 0, 0
            elif l > max_bits:
                raise ValueError("decode error: no symbol within max code length")
    raise ValueError("decode error: missing EOM")


def self_test(msgs, matcher, lengths, codes, count, first, group_offset, symbols, max_bits, ngrams):
    total_in_bits = 0
    total_out_bits = 0
    for msg in msgs:
        syms = tokenize(msg, matcher)
        data, bits = encode_bits(syms, lengths, codes)
        decoded = decode_stream(data, count, first, group_offset, symbols, max_bits, ngrams)
        if decoded != msg:
            raise AssertionError(f"round-trip mismatch:\n  in : {msg!r}\n  out: {decoded!r}")
        total_in_bits += len(msg) * 8
        total_out_bits += bits
    return total_in_bits, total_out_bits


# ── Header emission ─────────────────────────────────────────────────────────────

def c_escape(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif 0x20 <= ord(ch) <= 0x7E:
            out.append(ch)
        else:
            out.append(f"\\x{ord(ch):02x}")
    return "".join(out)


def sym_comment(sym, ngrams):
    if sym < 0x20:
        return CONTROL_NAMES[sym] + (" (EOM)" if sym == EOM else "")
    if sym == 0x7F:
        return "DEL"
    if sym <= 0x7E:
        ch = chr(sym)
        return '"\'"' if ch == "'" else f"'{ch}'"
    return '"%s"' % c_escape(ngrams[sym - 0x80])


def fmt_rows(values, fmt, per_row, indent="    "):
    lines = []
    for i in range(0, len(values), per_row):
        row = ", ".join(fmt(v) for v in values[i:i + per_row])
        lines.append(indent + row + ("," if i + per_row < len(values) else ""))
    return "\n".join(lines)


def emit_header(path, sources, corpus_chars, ngrams, freq, lengths, codes,
                count, first, group_offset, symbols, max_bits,
                in_bits, out_bits):
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    max_ngram_len = max((len(g) for g in ngrams), default=0)
    saved = in_bits - out_bits
    L = []
    L.append("/**")
    L.append(" * huffman_table.h — Auto-generated by huffman_analysis.py --emit")
    L.append(f" * Generated : {now}")
    L.append(f" * Sources   : {', '.join(sources)}")
    L.append(f" * Corpus    : {corpus_chars:,} chars")
    L.append(f" * N-grams   : {len(ngrams)} (sorted lexicographically for binary search)")
    L.append(" * Alphabet  : 255 symbols — 0x00 EOM, 0x01-0x7E ASCII, 0x7F unused, 0x80-0xFE n-gram")
    L.append(f" * Max depth : {max_bits} bits")
    L.append(" *")
    L.append(" * Compression on corpus:")
    L.append(f" *   Original  : {in_bits:,} bits ({in_bits // 8:,} bytes)")
    L.append(f" *   Compressed: {out_bits:,} bits ({(out_bits + 7) // 8:,} bytes, {out_bits * 100 / in_bits:.1f}% of original)")
    L.append(f" *   Savings   : {saved:,} bits ({saved * 100 / in_bits:.1f}% reduction)")
    L.append(" *")
    L.append(" * HUFFMAN_TABLE entry (uint32_t):")
    L.append(" *   bits [31:27]  code length in bits")
    L.append(" *   bits [26: 0]  canonical Huffman code, MSB first, right-justified")
    L.append(" *")
    L.append(" * Encoder tokeniser (C pseudocode):")
    L.append(" *   uint8_t lo = pgm_read_byte(&NGRAM_FIRST[c]);")
    L.append(" *   uint8_t hi = pgm_read_byte(&NGRAM_LAST[c]);")
    L.append(" *   for (j = lo; j < hi; j++) — try candidate n-grams, pick longest match")
    L.append(" *   emit HUFFMAN_TABLE[0x80|j]  or  HUFFMAN_TABLE[c] for single char")
    L.append(" *")
    L.append(" * Decoder loop: see HuffmanDecoder.h — O(HUFF_MAX_CODE_BITS) per symbol.")
    L.append(" */")
    L.append("")
    L.append("#pragma once")
    L.append("#include <pgmspace.h>")
    L.append("#include <stdint.h>")
    L.append("")
    L.append(f"#define NGRAM_COUNT           {len(ngrams)}")
    L.append(f"#define NGRAM_MAX_LEN           {max_ngram_len}")
    L.append("#define HUFFMAN_ALPHABET_SIZE 255")
    L.append(f"#define HUFF_MAX_CODE_BITS     {max_bits}")
    L.append("")
    L.append("// N-gram string pool sorted lexicographically.")
    L.append(f"// Entry i → token byte (0x80 | i).  {max_ngram_len} chars + NUL, null-padded.")
    L.append("// Use NGRAM_FIRST[c]/NGRAM_LAST[c] to find candidates for a given first char.")
    L.append("// clang-format off")
    L.append(f"static const char NGRAM_TABLE[NGRAM_COUNT][NGRAM_MAX_LEN + 1] PROGMEM = {{")
    for i, g in enumerate(ngrams):
        tok = 0x80 | i
        hlen = lengths.get(tok, 0)
        sep = "," if i < len(ngrams) - 1 else " "
        L.append(f'    "{c_escape(g)}"{sep}  // [{i:3d}] 0x{tok:02x}  hlen={hlen:2d}  n={len(g)}')
    L.append("};")
    L.append("// clang-format on")
    L.append("")
    L.append("// Length of each n-gram (chars, excl. NUL terminator).")
    L.append("static const uint8_t NGRAM_LEN[NGRAM_COUNT] PROGMEM = {")
    L.append(fmt_rows([len(g) for g in ngrams], lambda v: str(v), 16))
    L.append("};")
    L.append("")
    ngram_first = [255] * 128
    ngram_last = [255] * 128
    for i, g in enumerate(ngrams):
        c = ord(g[0])
        if ngram_first[c] == 255:
            ngram_first[c] = i
        ngram_last[c] = i + 1
    L.append("// NGRAM_FIRST[c]: first index in NGRAM_TABLE of n-grams starting with char c.")
    L.append("// 255 = no n-gram starts with this character.")
    L.append("static const uint8_t NGRAM_FIRST[128] PROGMEM = {")
    L.append(fmt_rows(ngram_first, lambda v: f"{v:3d}", 16))
    L.append("};")
    L.append("")
    L.append("// NGRAM_LAST[c]: exclusive end index for n-grams starting with char c.")
    L.append("// 255 = no n-gram starts with this character.")
    L.append("static const uint8_t NGRAM_LAST[128] PROGMEM = {")
    L.append(fmt_rows(ngram_last, lambda v: f"{v:3d}", 16))
    L.append("};")
    L.append("")
    L.append("// Huffman code table indexed by token byte value (255 entries).")
    L.append("// bits[31:27] = code length in bits,  bits[26:0] = canonical code (MSB first).")
    L.append("// clang-format off")
    L.append("static const uint32_t HUFFMAN_TABLE[HUFFMAN_ALPHABET_SIZE] PROGMEM = {")
    for sym in range(255):
        if sym in lengths:
            entry = (lengths[sym] << 27) | codes[sym]
            desc = sym_comment(sym, ngrams)
            comment = f"0x{sym:02x} {desc:<20} len={lengths[sym]:2d}  freq={freq[sym]:,}"
        else:
            entry = 0
            desc = sym_comment(sym, ngrams) if sym < 0x80 else "[unused n-gram slot]"
            comment = f"0x{sym:02x} {desc}  (unused)"
        sep = "," if sym < 254 else " "
        L.append(f"    0x{entry:08X}{sep}  // {comment}")
    L.append("};")
    L.append("// clang-format on")
    L.append("")
    L.append("// Canonical Huffman decode tables.")
    L.append("// Decoder: accumulate one bit per step; at length L check")
    L.append("//   if code in [FIRST[L], FIRST[L]+COUNT[L]):")
    L.append("//     symbol = SYMBOLS[GROUP[L] + code - FIRST[L]]")
    L.append("")
    L.append("// Number of codes at each bit-length (index 0 unused).")
    L.append("static const uint8_t HUFF_DECODE_COUNT[HUFF_MAX_CODE_BITS + 1] PROGMEM = {")
    L.append("    " + ", ".join(str(c) for c in count))
    L.append("};")
    L.append("")
    L.append("// First canonical code value at each bit-length.")
    L.append("static const uint32_t HUFF_DECODE_FIRST[HUFF_MAX_CODE_BITS + 1] PROGMEM = {")
    L.append("    " + ", ".join(f"0x{v:08X}" for v in first))
    L.append("};")
    L.append("")
    L.append("// Starting index in HUFF_DECODE_SYMBOLS for each bit-length.")
    L.append("static const uint16_t HUFF_DECODE_GROUP_OFFSET[HUFF_MAX_CODE_BITS + 1] PROGMEM = {")
    L.append("    " + ", ".join(str(v) for v in group_offset))
    L.append("};")
    L.append("")
    L.append(f"// Symbols in canonical order ({len(symbols)} entries).")
    L.append(f"static const uint8_t HUFF_DECODE_SYMBOLS[{len(symbols)}] PROGMEM = {{")
    L.append(fmt_rows(symbols, lambda v: f"0x{v:02x}", 16))
    L.append("};")
    L.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print(f"Wrote {path}")


# ── Main ────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=DEFAULT_SERVER,
                    help=f"lightinator-log-service base URL (default: {DEFAULT_SERVER})")
    ap.add_argument("--ips", help="comma-separated controller IPs (default: all known controllers)")
    ap.add_argument("--max-messages", type=int, default=100000,
                    help="max messages to fetch per controller (default: 100000)")
    ap.add_argument("--corpus", help="read corpus from a local file instead of the log service")
    ap.add_argument("--save-corpus", help="save the fetched corpus to a file for reuse")
    ap.add_argument("--ngrams", action="store_true", default=True,
                    help="enable n-gram token selection (default: on)")
    ap.add_argument("--no-ngrams", dest="ngrams", action="store_false",
                    help="chars-only Huffman code, no n-gram tokens (much worse ratio)")
    ap.add_argument("--ngram-count", type=int, default=127,
                    help="number of n-grams to select, max 127 (default: 127)")
    ap.add_argument("--ngram-max-len", type=int, default=8,
                    help="maximum n-gram length (default: 8)")
    ap.add_argument("--emit", action="store_true", help="emit the C header")
    ap.add_argument("--out", default="huffman_table.h", help="output header path (default: huffman_table.h)")
    args = ap.parse_args()

    if args.ngram_count > 127:
        sys.exit("error: --ngram-count must be <= 127 (token bytes 0x80-0xFE)")

    msgs, sources = load_corpus(args)
    corpus_chars = sum(len(m) for m in msgs)
    print(f"Corpus: {len(msgs):,} messages, {corpus_chars:,} chars")

    ngrams = []
    if args.ngrams:
        print("Selecting n-grams…")
        ngrams = select_ngrams(msgs, args.ngram_count, args.ngram_max_len)
        print(f"Selected {len(ngrams)} n-grams")
    else:
        print("note: --no-ngrams — chars-only code, expect a much worse ratio")

    matcher = build_matcher(ngrams)
    print("Tokenising corpus…")
    freq = symbol_frequencies(msgs, matcher, len(ngrams))

    lengths = code_lengths(freq)
    codes, count, first, max_bits = canonical_codes(lengths)
    group_offset, symbols = decode_tables(lengths, count, first, max_bits)
    print(f"Huffman: {len(lengths)} coded symbols, max depth {max_bits} bits")

    print("Running round-trip self-test…")
    in_bits, out_bits = self_test(msgs, matcher, lengths, codes, count, first,
                                  group_offset, symbols, max_bits, ngrams)
    print(f"Self-test OK — {len(msgs):,} messages round-tripped")
    print(f"Compression: {in_bits:,} → {out_bits:,} bits "
          f"({out_bits * 100 / in_bits:.1f}% of original, {100 - out_bits * 100 / in_bits:.1f}% saved)")

    if args.emit:
        emit_header(args.out, sources, corpus_chars, ngrams, freq, lengths, codes,
                    count, first, group_offset, symbols, max_bits, in_bits, out_bits)
    else:
        print("(dry run — pass --emit to write the header)")


if __name__ == "__main__":
    main()
