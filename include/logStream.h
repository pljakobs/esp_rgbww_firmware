#pragma once
#include <SmingCore.h>
#include <app-log.h>

/**
 * @brief A Stream class that logs to both Serial and ConfigDB
 * 
 * This stream writes all output to Serial immediately and buffers
 * incoming data to write complete lines to the ConfigDB log database.
 * Single character writes are buffered until a newline or the buffer
 * is full (80 characters).
 */
class LogStream : public Stream
{
public:
    LogStream();
    ~LogStream();

    // Stream interface implementation
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override;

private:
    static const size_t BUFFER_SIZE = 80;
    
    char lineBuffer[BUFFER_SIZE + 1]; // +1 for null terminator
    size_t bufferIndex;
    
    void flushLine();
    void writeToDatabase(const String& message);
};