// test_roundtrip — host-side round-trip test for the HuffmanCodec component.
//
// Compiles the REAL firmware sources (HuffmanEncoder/HuffmanDecoder/
// HuffmanRingBuffer) against a huffman_table.h selected at build time
// (make TABLE_DIR=...), encodes every stdin line, decodes it again and
// verifies the result byte-for-byte.
//
// Input : one message per line on stdin; literal "\x1b" unescapes to ESC
//         (the format written by huffman_analysis.py --save-corpus).
//         Lines longer than the encoder input buffer are truncated to match
//         firmware behaviour before comparison.
// Output: with --dump, one lowercase hex compressed frame per line on stdout
//         (empty line for messages that produce no frame) for cross-checking
//         against the Python implementation; summary always goes to stderr.
//
// Exit code 0 = all messages round-tripped, 1 = at least one mismatch.

#include <cstdio>
#include <cstring>
#include <string>

#include "HuffmanEncoder.h"
#include "HuffmanDecoder.h"
#include "HuffmanRingBuffer.h"
#include "huffman_table.h"

static std::string unescape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for(size_t i = 0; i < in.size();) {
        if(in.compare(i, 4, "\\x1b") == 0) {
            out.push_back('\x1b');
            i += 4;
        } else {
            out.push_back(in[i++]);
        }
    }
    return out;
}

int main(int argc, char** argv)
{
    bool dump = (argc > 1 && strcmp(argv[1], "--dump") == 0);

    static uint8_t ringMem[4096];
    HuffmanRingBuffer rb(ringMem, sizeof(ringMem));
    HuffmanEncoder encoder(rb);

    unsigned long total = 0, failed = 0, inBytes = 0, outBytes = 0;
    char line[8192];

    while(fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while(len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if(len == 0) {
            if(dump) puts("");
            continue;
        }

        std::string msg = unescape(line);
        // The decoder guarantees full round-trip only up to
        // DECODE_BUF_SIZE - NGRAM_MAX_LEN chars (it reserves n-gram expansion
        // headroom), so mirror that end-to-end limit here.
        constexpr size_t maxMsg = HuffmanDecoder::DECODE_BUF_SIZE - NGRAM_MAX_LEN;
        if(msg.size() > maxMsg) {
            msg.resize(maxMsg);
        }

        total++;
        encoder.print(msg.c_str());
        encoder.flush(); // encode + append EOM + push to ring buffer

        uint8_t frame[HuffmanDecoder::COMP_BUF_SIZE];
        uint16_t frameLen = 0;
        if(!rb.read(frame, sizeof(frame), frameLen)) {
            fprintf(stderr, "FAIL (no frame): %s\n", line);
            failed++;
            if(dump) puts("");
            continue;
        }

        char decoded[HuffmanDecoder::DECODE_BUF_SIZE];
        uint16_t decLen = HuffmanDecoder::decodeFrame(frame, frameLen, decoded, sizeof(decoded));

        if(decLen != msg.size() || memcmp(decoded, msg.data(), decLen) != 0) {
            fprintf(stderr, "FAIL (mismatch): %s\n", line);
            failed++;
        }

        inBytes += msg.size();
        outBytes += frameLen;

        if(dump) {
            for(uint16_t i = 0; i < frameLen; i++) {
                printf("%02x", frame[i]);
            }
            putchar('\n');
        }
    }

    fprintf(stderr, "C round-trip: %lu messages, %lu failed, %lu -> %lu bytes (%.1f%%)\n",
            total, failed, inBytes, outBytes,
            inBytes ? 100.0 * outBytes / inBytes : 0.0);
    return failed ? 1 : 0;
}
