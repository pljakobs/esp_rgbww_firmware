#pragma once

#include <stdint.h>

/**
 * @brief Ring buffer of compressed log messages backed by a caller-supplied memory block.
 *
 * Each message is stored as:
 *   [uint16_t compressed_byte_len (LE)] [compressed bytes ...]
 *
 * When the buffer is full, the oldest message is evicted automatically to make room.
 * Not ISR-safe; add external locking if called from multiple contexts.
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

    bool     empty()    const { return _count == 0; }
    uint16_t count()    const { return _count; }
    uint16_t used()     const { return _used; }
    uint16_t capacity() const { return _capacity; }

private:
    uint8_t*  _buf;
    uint16_t  _capacity;
    uint16_t  _head;   ///< byte offset of oldest message header
    uint16_t  _tail;   ///< byte offset where next write begins
    uint16_t  _used;   ///< bytes currently occupied
    uint16_t  _count;  ///< number of messages stored

    // Low-level helpers — all offsets must be in [0, _capacity)
    uint16_t _readU16(uint16_t offset) const;
    void     _writeU16(uint16_t offset, uint16_t val);
    void     _copyIn(uint16_t dst, const uint8_t* src, uint16_t len);
    void     _copyOut(uint8_t* dst, uint16_t src, uint16_t len) const;

    void _evictOldest();
};
