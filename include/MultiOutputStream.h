/**
 * @author  Peter Jakobs http://github.com/pljakobs
 * 
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * https://www.gnu.org/copyleft/gpl.html
 */

#include <SmingCore.h>
#include <vector>
#include <algorithm>
#include <Data/Stream/FileStream.h>
#include <Data/Stream/MemoryDataStream.h>

struct BufferFileHeader {
    size_t totalMessages;
    size_t nextReadPosition;  // File position for next message to read
    size_t writePosition;     // File position for next message to write
};

class MultiOutputStream : public Stream
{
private:
    static const size_t MAX_BUFFERED_STREAMS = 8; // Bitmap supports up to 8 streams

public:
    MultiOutputStream() : minFreeHeap(8000)
    {
        
    }

    ~MultiOutputStream() 
    {
        
    }

    void addStream(Stream* stream, bool buffered = false)
    {
        
        // Check for duplicates
        for (auto existing : unbufferedStreams) {
            if (existing == stream) {
                Serial.println("MultiOutputStream: WARNING - Unbuffered stream already added!");
                return;
            }
        }
        
        unbufferedStreams.push_back(stream);
        Serial.printf("MultiOutputStream: Added unbuffered stream (total: %u)\n", unbufferedStreams.size());
    
    }

    void removeStream(Stream* stream)
    {
        // Remove from unbuffered streams
        unbufferedStreams.erase(std::remove(unbufferedStreams.begin(), unbufferedStreams.end(), stream), unbufferedStreams.end());
        
    }

    size_t write(uint8_t c) override
    {
        // EMERGENCY: Completely disable if memory is critically low
        if (system_get_free_heap_size() < 3000) {
            return 1; // Pretend success but do nothing
        }
        
        // Always write to unbuffered streams
        for (auto stream : unbufferedStreams)
        {
            stream->write(c);
        }
        
        return 1;
    }
    
    size_t ICACHE_FLASH_ATTR write(const uint8_t* buffer, size_t size) override
    {
        size_t freeHeap = system_get_free_heap_size();
        
        // EMERGENCY: Completely disable if memory is critically low
        if (freeHeap < 3000) {
            return size; // Pretend success but do nothing to prevent crash
        }
        
        // Always write to unbuffered streams (like Serial)
        for (auto stream : unbufferedStreams)
        {
            if (system_get_free_heap_size() < 3500) break; // Stop if memory drops during writes
            stream->write(buffer, size);
        }
        
        return size;
    }

    int available() override
    // no bytes available in a write only stream
    {
        return 0;
    }

    int read() override
    // read doesn't make sense in write only streams
    {
        return -1;
    }

    int peek() override
    // peek doesn't make sense in write only streams
    {
        return -1;
    }    

    void flush() override
    {
        for (auto stream : unbufferedStreams)
        {
            stream->flush();
        }
    }


private:

    std::vector<Stream*> unbufferedStreams;
    size_t minFreeHeap;
   

};