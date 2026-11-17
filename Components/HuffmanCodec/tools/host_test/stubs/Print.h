#pragma once
// Host-test stub for Sming's Print base class — just enough for HuffmanEncoder.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

class Print
{
public:
    virtual ~Print() = default;

    virtual size_t write(uint8_t c) = 0;

    virtual size_t write(const uint8_t* buffer, size_t size)
    {
        size_t n = 0;
        while(size--) {
            n += write(*buffer++);
        }
        return n;
    }

    size_t print(const char* s)
    {
        return write(reinterpret_cast<const uint8_t*>(s), strlen(s));
    }
};
