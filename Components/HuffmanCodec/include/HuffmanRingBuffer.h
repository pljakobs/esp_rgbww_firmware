#pragma once

#include <stdint.h>

/**
 * @brief Ring buffer of Huffman-compressed log lines backed by a caller-supplied memory block.
 *
 * ## Concept
 *
 * Each entry in the buffer stores one complete log line — i.e. the bytes that arrived
 * between two successive newline characters on the debug output stream.  Before being
 * stored, each line is n-gram tokenised and Huffman-encoded by HuffmanEncoder::flush();
 * the resulting compressed bitstream (padded to a byte boundary) is what this buffer holds.
 * HuffmanDecoder::decodeFrame() reverses the process, producing the original plaintext line.
 *
 * ## Memory layout
 *
 * The backing store is a flat byte array used as a circular/wrap-around queue.
 * `_head` points to the oldest entry; `_tail` points to where the next entry will be written.
 * Each entry is a self-describing length-prefixed record:
 *
 * @code
 * Byte offset (relative to entry start):
 *   [0]      low byte  of compressed_len  (uint16_t little-endian)
 *   [1]      high byte of compressed_len
 *   [2 .. 2+compressed_len-1]   Huffman-compressed bytes (bit-packed, MSB first)
 *
 * Example — two entries, capacity = 32 bytes, _head = 0, _tail = 20, _used = 20:
 *
 *  offset:  0    1    2    3 ..  8    9   10   11   12 .. 19   20  21 ..
 *           [len_lo][len_hi][  compressed bytes  ][len_lo][len_hi][ comp ] <- _tail
 *            <------- entry 0, len=6 ------------>  <---- entry 1,len=8 ->
 *             ^_head                                                        ^_tail
 * @endcode
 *
 * The buffer wraps: when `_tail + new_entry_size > capacity`, bytes are stored modulo
 * `capacity` (i.e. the header or payload may straddle the end of the array).
 *
 * ## Eviction policy
 *
 * When a push() would overflow the buffer, the oldest entry (at `_head`) is silently
 * evicted — its length is read, `_head` and `_used` are updated, and `_count` is
 * decremented.  This is repeated **in a loop** until there is enough cumulative space
 * for the new entry.  A single incoming message can therefore cause multiple older
 * messages to be evicted if it is longer than any one of them.  A push() fails only
 * if the new entry is larger than the entire capacity (checked upfront).
 *
 * ## Thread / ISR safety
 *
 * Not ISR-safe.  Access must be serialised externally if called from multiple contexts.
 */
class HuffmanRingBuffer
{
public:
    /**
     * @param mem       Caller-allocated memory block (lifetime must exceed this object)
     * @param capacity  Size of the memory block in bytes (minimum ~8)
     */
    HuffmanRingBuffer(uint8_t* mem, uint16_t capacity);

    /**
     * @brief Push a compressed message into the ring buffer.
     *
     * Evicts the oldest message(s) as needed.
     * @return false if `len + 2 > capacity` (message can never fit)
     */
    bool push(const uint8_t* data, uint16_t len);

    /**
     * @brief Copy the oldest message without removing it.
     * @param out     Destination buffer
     * @param maxLen  Size of destination buffer
     * @param outLen  Number of bytes written on success
     * @return false if empty or outLen > maxLen
     */
    bool peek(uint8_t* out, uint16_t maxLen, uint16_t& outLen) const;

    /**
     * @brief Remove the oldest message (does not copy it out).
     * @return false if buffer is empty
     */
    bool pop();

    /**
     * @brief Copy oldest message AND remove it in one call.
     * @return false if empty or message too large for `out`
     */
    bool read(uint8_t* out, uint16_t maxLen, uint16_t& outLen);

    bool     empty()        const { return _count == 0; }
    uint16_t count()        const { return _count; }
    uint16_t used()         const { return _used; }
    uint16_t capacity()     const { return _capacity; }
    uint16_t evictedCount() const { return _evictedCount; }

    /**
     * @brief Return the compressed byte length of the message at the given index.
     *
     * Index 0 is the oldest (next to be read) message.  O(index) traversal —
     * suitable for diagnostic iteration over a small number of messages.
     * @param index  0-based message index.
     * @return       Compressed byte count, or 0 if @p index >= count().
     */
    uint16_t messageLen(uint16_t index) const;

private:
    uint8_t*  _buf;
    uint16_t  _capacity;
    uint16_t  _head;         ///< byte offset of oldest message header
    uint16_t  _tail;         ///< byte offset where next write begins
    uint16_t  _used;         ///< bytes currently occupied
    uint16_t  _count;        ///< number of messages stored
    uint16_t  _evictedCount; ///< total messages dropped due to buffer wraparound

    // Low-level helpers — all offsets must be in [0, _capacity)
    uint16_t _readU16(uint16_t offset) const;
    void     _writeU16(uint16_t offset, uint16_t val);
    void     _copyIn(uint16_t dst, const uint8_t* src, uint16_t len);
    void     _copyOut(uint8_t* dst, uint16_t src, uint16_t len) const;

    void _evictOldest();
};
