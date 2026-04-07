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
    /**
     * @brief Copy up to `bufSize` decoded plaintext bytes into `data`.
     * @return Number of bytes written; 0 if no data is available.
     */
    uint16_t readMemoryBlock(char* data, int bufSize) override;

    /**
     * @brief Advance the read cursor by `len` bytes.
     * @return true on success, false if `len` exceeded available bytes.
     */
    bool seek(int len) override;

    /** @return true when the ring buffer is empty and the current message has been fully read. */
    bool isFinished() override;

    /** @return Number of decoded bytes remaining in the current message, or -1 if nothing loaded. */
    int available() override;

    // ── ReadWriteStream — write side intentionally not implemented ────────────
    size_t write(const uint8_t*, size_t) override { return 0; }

private:
    HuffmanRingBuffer& _rb;

    uint8_t  _decodedBuf[DECODE_BUF_SIZE]; ///< Plaintext staging area for the current message.
    uint16_t _decodedLen;  ///< Bytes in decoded buffer.
    uint16_t _decodedPos;  ///< Read cursor within decoded buffer.

    uint8_t  _compBuf[COMP_BUF_SIZE]; ///< Staging buffer for one compressed message.

    // ── Bit-level decode state (valid only during _decodeMessage) ─────────────
    const uint8_t* _bits;    ///< Pointer to compressed bytes being decoded.
    uint16_t       _bitLen;  ///< Total bits available (lenBytes * 8).
    uint16_t       _bitPos;  ///< Current bit position (bit 7 of byte 0 = position 0).

    /** @return true if more bits remain in the current compressed frame. */
    bool     _hasBit()        const;

    /**
     * @brief Consume and return the next bit from the compressed bitstream.
     * @pre _hasBit() must be true.
     */
    uint8_t  _nextBit();

    /**
     * @brief Decode one symbol from the current bitstream using the canonical Huffman table.
     * @return Decoded symbol byte, or 0xFFFF on decode error / exhausted input.
     */
    uint16_t _decodeSymbol();

    /**
     * @brief Load, decode, and cache the next compressed message from the ring buffer.
     * @return true if a non-empty message was decoded; false if the buffer is empty.
     */
    bool     _loadNextMessage();

    /**
     * @brief Decode a raw compressed frame into _decodedBuf.
     * @param data      Compressed bytes.
     * @param lenBytes  Number of compressed bytes.
     * @return          Number of decoded bytes written to _decodedBuf.
     */
    uint16_t _decodeMessage(const uint8_t* data, uint16_t lenBytes);
};
