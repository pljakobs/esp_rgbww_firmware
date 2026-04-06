#include "../include/HuffmanRingBuffer.h"
#include <string.h>

HuffmanRingBuffer::HuffmanRingBuffer(uint8_t* mem, uint16_t capacity)
    : _buf(mem), _capacity(capacity), _head(0), _tail(0), _used(0), _count(0)
{
}

// ── Low-level helpers ─────────────────────────────────────────────────────────

uint16_t HuffmanRingBuffer::_readU16(uint16_t offset) const
{
    uint16_t hi = offset + 1;
    if(hi >= _capacity) hi -= _capacity;
    return (uint16_t)_buf[offset] | ((uint16_t)_buf[hi] << 8);
}

void HuffmanRingBuffer::_writeU16(uint16_t offset, uint16_t val)
{
    uint16_t hi = offset + 1;
    if(hi >= _capacity) hi -= _capacity;
    _buf[offset] = (uint8_t)(val & 0xFF);
    _buf[hi]     = (uint8_t)(val >> 8);
}

void HuffmanRingBuffer::_copyIn(uint16_t dst, const uint8_t* src, uint16_t len)
{
    for(uint16_t i = 0; i < len; i++) {
        _buf[dst] = src[i];
        if(++dst == _capacity) dst = 0;
    }
}

void HuffmanRingBuffer::_copyOut(uint8_t* dst, uint16_t src, uint16_t len) const
{
    for(uint16_t i = 0; i < len; i++) {
        dst[i] = _buf[src];
        if(++src == _capacity) src = 0;
    }
}

void HuffmanRingBuffer::_evictOldest()
{
    if(_count == 0) return;
    uint16_t msgLen = _readU16(_head);
    uint16_t total  = 2u + msgLen;
    _head += total;
    if(_head >= _capacity) _head -= _capacity;
    _used -= total;
    _count--;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool HuffmanRingBuffer::push(const uint8_t* data, uint16_t len)
{
    uint16_t total = 2u + len;
    if(total > _capacity) return false; // will never fit

    // Evict oldest messages until there is room
    while((_capacity - _used) < total) {
        if(_count == 0) return false; // defensive; shouldn't be reached
        _evictOldest();
    }

    _writeU16(_tail, len);
    uint16_t dataStart = _tail + 2;
    if(dataStart >= _capacity) dataStart -= _capacity;
    _copyIn(dataStart, data, len);

    _tail += total;
    if(_tail >= _capacity) _tail -= _capacity;
    _used  += total;
    _count++;
    return true;
}

bool HuffmanRingBuffer::peek(uint8_t* out, uint16_t maxLen, uint16_t& outLen) const
{
    if(_count == 0) return false;
    outLen = _readU16(_head);
    if(outLen > maxLen) return false;
    uint16_t src = _head + 2;
    if(src >= _capacity) src -= _capacity;
    _copyOut(out, src, outLen);
    return true;
}

bool HuffmanRingBuffer::pop()
{
    if(_count == 0) return false;
    _evictOldest();
    return true;
}

bool HuffmanRingBuffer::read(uint8_t* out, uint16_t maxLen, uint16_t& outLen)
{
    if(!peek(out, maxLen, outLen)) return false;
    _evictOldest();
    return true;
}
