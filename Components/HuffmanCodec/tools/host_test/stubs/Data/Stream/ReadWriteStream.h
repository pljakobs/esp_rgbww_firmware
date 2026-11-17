#pragma once
// Host-test stub for Sming's ReadWriteStream — just enough for HuffmanDecoder.
#include <stddef.h>
#include <stdint.h>

class ReadWriteStream
{
public:
    virtual ~ReadWriteStream() = default;

    virtual uint16_t readMemoryBlock(char* data, int bufSize) = 0;
    virtual bool seek(int len) = 0;
    virtual bool isFinished() = 0;
    virtual int available() = 0;
    virtual size_t write(const uint8_t* buffer, size_t size) = 0;
};
