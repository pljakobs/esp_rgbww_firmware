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

    /**
     * @brief Buffer one character for encoding.
     *
     * Characters are accumulated until flush() is called.  Writing a NUL (0x00)
     * triggers flush() automatically.  Input silently truncated if the buffer is
     * full (INPUT_BUF_SIZE bytes).
     * @param c  Byte to buffer
     * @return   Always 1
     */
    size_t write(uint8_t c) override;

    /**
     * @brief Buffer a block of bytes for encoding.
     *
     * Calls write(uint8_t) for each byte in [data, data+size).
     * @param data  Source bytes
     * @param size  Number of bytes to buffer
     * @return      Always `size`
     */
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
    /// @brief Underlying ring buffer reference.
    HuffmanRingBuffer& _rb;

    uint8_t  _inBuf[INPUT_BUF_SIZE];  ///< Unencoded input staging area.
    uint16_t _inLen;                   ///< Number of bytes currently in _inBuf.

    uint8_t  _outBuf[OUTPUT_BUF_SIZE]; ///< Bit-packed compressed output staging area.
    uint16_t _outLen;                  ///< Number of bytes written into _outBuf.
    uint8_t  _bitBuf;  ///< partial output byte being assembled (MSB first)
    uint8_t  _bitPos;  ///< bits filled in _bitBuf (0 = empty)

    /**
     * @brief Encode one token byte via HUFFMAN_TABLE and append its bits to _outBuf.
     * @param sym  Token byte: 0x01-0x7E = ASCII, 0x80-0xFE = n-gram, 0x00 = EOM
     */
    void _encodeSymbol(uint8_t sym);

    /**
     * @brief Append a single bit to _outBuf (MSB-first packing).
     *
     * Flushes the current byte to _outBuf and resets _bitBuf/_bitPos whenever
     * 8 bits have been accumulated.
     * @param bit  Bit value (only LSB is used)
     */
    void _pushBit(uint8_t bit);
};
