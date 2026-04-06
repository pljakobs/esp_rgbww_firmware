#pragma once

#include <Print.h>
#include "HuffmanRingBuffer.h"

/**
 * @brief Encodes text log messages with Huffman + n-gram tokenisation and
 *        pushes compressed frames into a HuffmanRingBuffer.
 *
 * Requires the generated huffman_table.h (NGRAM_TABLE, NGRAM_FIRST, NGRAM_LAST,
 * NGRAM_LEN, HUFFMAN_TABLE) to be on the compiler include path.
 *
 * Usage:
 * @code
 *   encoder.print("my log message");
 *   encoder.flush();          // encode, append NUL terminator, push to ring buffer
 *   // — or —
 *   encoder.write('\0');      // writing NUL triggers flush automatically
 * @endcode
 *
 * The encoder buffers input until flush().  If the input buffer overflows
 * (> INPUT_BUF_SIZE - 1 bytes between flushes) the oldest buffered bytes are
 * silently dropped to leave room for the NUL terminator.
 */
class HuffmanEncoder : public Print
{
public:
    static constexpr uint16_t INPUT_BUF_SIZE  = 512; ///< max chars per message
    static constexpr uint16_t OUTPUT_BUF_SIZE = 512; ///< max compressed bytes per message

    explicit HuffmanEncoder(HuffmanRingBuffer& rb);

    size_t write(uint8_t c) override;
    size_t write(const uint8_t* data, size_t size) override;
    using Print::write;

    /**
     * @brief Tokenise + encode the buffered input, append NUL end-of-message,
     *        pad to byte boundary, and push to the ring buffer.
     * @return Compressed byte count stored, 0 on failure or empty input.
     */
    uint16_t flush();

    /** @brief Discard accumulated input without encoding. */
    void discard() { _inLen = 0; }

private:
    HuffmanRingBuffer& _rb;

    uint8_t  _inBuf[INPUT_BUF_SIZE];
    uint16_t _inLen;

    uint8_t  _outBuf[OUTPUT_BUF_SIZE];
    uint16_t _outLen;
    uint8_t  _bitBuf;  ///< partial output byte being assembled (MSB first)
    uint8_t  _bitPos;  ///< bits filled in _bitBuf (0 = empty)

    /// Encode one token byte via HUFFMAN_TABLE and push its bits to _outBuf.
    void _encodeSymbol(uint8_t sym);
    /// Push a single bit into _outBuf (MSB first packing).
    void _pushBit(uint8_t bit);
};
