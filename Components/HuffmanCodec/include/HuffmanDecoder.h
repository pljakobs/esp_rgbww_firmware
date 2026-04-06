#pragma once

#include <Data/Stream/ReadWriteStream.h>
#include "HuffmanRingBuffer.h"

/**
 * @brief Reads compressed messages from a HuffmanRingBuffer, decodes them,
 *        and presents the plaintext as a ReadWriteStream.
 *
 * Requires the generated huffman_table.h (NGRAM_TABLE, NGRAM_LEN,
 * HUFF_DECODE_COUNT, HUFF_DECODE_FIRST, HUFF_DECODE_GROUP_OFFSET,
 * HUFF_DECODE_SYMBOLS, HUFF_MAX_CODE_BITS) to be on the include path.
 *
 * Messages are fed out sequentially from oldest to newest.
 * Each decoded message is NUL-terminated in the output stream.
 * isFinished() returns true when the ring buffer is empty and the current
 * decoded message has been fully read.
 *
 * Decoder algorithm (O(HUFF_MAX_CODE_BITS) per output symbol):
 * @code
 *   code = 0
 *   for L = 1 .. HUFF_MAX_CODE_BITS:
 *     code = (code << 1) | next_bit()
 *     if code in [FIRST[L], FIRST[L] + COUNT[L]):
 *       sym = SYMBOLS[GROUP[L] + code - FIRST[L]]
 *       if sym == 0x00: end-of-message
 *       if sym <= 0x7E: output ASCII char
 *       if sym >= 0x80: expand NGRAM_TABLE[sym - 0x80]
 * @endcode
 */
class HuffmanDecoder : public ReadWriteStream
{
public:
    static constexpr uint16_t DECODE_BUF_SIZE = 512; ///< max decoded bytes per message
    static constexpr uint16_t COMP_BUF_SIZE   = 512; ///< max compressed bytes per message

    explicit HuffmanDecoder(HuffmanRingBuffer& rb);

    /**
     * @brief Standalone one-shot decode of a single compressed frame.
     *
     * Decodes a frame produced by HuffmanEncoder::flush() into plaintext.
     * Does NOT require a HuffmanRingBuffer — intended for use in replay drain
     * (e.g. UdpSyslogStream::begin()).  Does not append a NUL terminator.
     *
     * @param data      Compressed frame bytes (from HuffmanRingBuffer::read)
     * @param lenBytes  Frame length in bytes
     * @param out       Output character buffer
     * @param outMax    Output buffer capacity
     * @return          Number of decoded chars written to out (no NUL)
     */
    static uint16_t decodeFrame(const uint8_t* data, uint16_t lenBytes,
                                char* out, uint16_t outMax);

    // ── IDataSourceStream ─────────────────────────────────────────────────────
    uint16_t readMemoryBlock(char* data, int bufSize) override;
    bool     seek(int len) override;
    bool     isFinished() override;
    int      available() override;

    // ── ReadWriteStream — write side intentionally not implemented ────────────
    size_t write(const uint8_t*, size_t) override { return 0; }

private:
    HuffmanRingBuffer& _rb;

    uint8_t  _decodedBuf[DECODE_BUF_SIZE];
    uint16_t _decodedLen;  ///< bytes in decoded buffer
    uint16_t _decodedPos;  ///< read cursor within decoded buffer

    uint8_t  _compBuf[COMP_BUF_SIZE]; ///< staging buffer for one compressed message

    // ── Bit-level decode state (valid during _decodeMessage) ─────────────────
    const uint8_t* _bits;    ///< pointer to compressed bytes being decoded
    uint16_t       _bitLen;  ///< total bits available
    uint16_t       _bitPos;  ///< current bit position (MSB of byte 0 = bit 0)

    bool     _hasBit()        const;
    uint8_t  _nextBit();
    uint16_t _decodeSymbol();

    bool     _loadNextMessage();
    uint16_t _decodeMessage(const uint8_t* data, uint16_t lenBytes);
};
