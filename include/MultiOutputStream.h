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
    MultiOutputStream() : minFreeHeap(8000), bufferFilePath("/tmp/stream_buffer.fifo"), 
                         flushIntervalMs(100), flushCounter(0), isFileOpen(false)
    {
        Serial.println("MultiOutputStream: Initializing file-based buffering...");
        
        // Initialize file header info without loading messages
        initializeBufferFile();
        
        Serial.printf("MultiOutputStream: Buffer file ready, timer interval: %lums\n", flushIntervalMs);
        
        // Start periodic flush timer
        //flushTimer.initializeMs(flushIntervalMs, TimerDelegate(&MultiOutputStream::flushBufferedData, this));
        //flushTimer.start();
        
        Serial.println("MultiOutputStream: All buffered streams use file-based delivery");
    }

    ~MultiOutputStream() 
    {
        Serial.println("MultiOutputStream: Destructor called");
        if (isFileOpen) {
            bufferFile.close();
            isFileOpen = false;
        }
    }

    void addStream(Stream* stream, bool buffered = false)
    {
        if (buffered)
        {
            if (bufferedStreams.size() >= MAX_BUFFERED_STREAMS) {
                Serial.printf("MultiOutputStream: Cannot add more than %u buffered streams", MAX_BUFFERED_STREAMS);
                return;
            }
            
            // Check for duplicates
            for (auto existing : bufferedStreams) {
                if (existing == stream) {
                    Serial.println("MultiOutputStream: WARNING - Buffered stream already added!");
                    return;
                }
            }
            
            bufferedStreams.push_back(stream);
            Serial.printf("MultiOutputStream: Added buffered stream (total: %u)\n", bufferedStreams.size());
        }
        else
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
    }

    void removeStream(Stream* stream)
    {
        // Remove from unbuffered streams
        unbufferedStreams.erase(std::remove(unbufferedStreams.begin(), unbufferedStreams.end(), stream), unbufferedStreams.end());
        
        // Remove from buffered streams
        bufferedStreams.erase(std::remove(bufferedStreams.begin(), bufferedStreams.end(), stream), bufferedStreams.end());
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
        
        // For buffered streams, decide whether to write directly or buffer
        if (!bufferedStreams.empty()) {
            size_t currentHeap = system_get_free_heap_size();
            if (currentHeap > minFreeHeap) {
                // Plenty of memory - write directly to all buffered streams
                for (auto stream : bufferedStreams) {
                    stream->write(c);
                }
            } else {
                // Low memory - use buffering mechanism
                String data((char)c);
                handleBufferedWrite(data);
            }
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
        
        // For buffered streams, decide whether to write directly or buffer
        if (size > 1 && !bufferedStreams.empty()) {
            String data((const char*)buffer, size);
            
            if (freeHeap > minFreeHeap) {
                // Plenty of memory - write directly to all buffered streams
                // Serial.printf("MultiOutputStream: Direct write to %u buffered streams (free: %u > %u)\n", 
                //             bufferedStreams.size(), freeHeap, minFreeHeap);
                for (auto stream : bufferedStreams) {
                    if (system_get_free_heap_size() < 3500) break; // Stop if memory drops during writes
                    stream->write(buffer, size);
                }
            } else {
                // Low memory - use buffering mechanism
                Serial.printf("MultiOutputStream: Low memory, buffering message (free: %u <= %u)\n", freeHeap, minFreeHeap);
                handleBufferedWrite(data);
            }
        }
        
        return size;
    }

    int available() override
    // no bytes available in a read only stream
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
        for (auto stream : bufferedStreams)
        {
            stream->flush();
        }
    }

    // Buffer management functions
    void printBufferContents(Stream* output = &Serial)
    {
        if (!isFileOpen) {
            output->println("MultiOutputStream: Buffer file not open");
            return;
        }
        
        // Save current position
        size_t originalPos = bufferFile.getPos();
        
        // Read and print header
        bufferFile.seek(0);
        BufferFileHeader header;
        if (bufferFile.readMemoryBlock(reinterpret_cast<char*>(&header), sizeof(header)) == sizeof(header)) {
            output->printf("Buffer file status:\n");
            output->printf("  Total messages: %u\n", header.totalMessages);
            output->printf("  Next read pos: %u\n", header.nextReadPosition);
            output->printf("  Write pos: %u\n", header.writePosition);
            output->printf("  File size: %u bytes\n", bufferFile.getSize());
        } else {
            output->println("Failed to read buffer header");
        }
        
        // Restore original position
        bufferFile.seek(originalPos);
    }
    
    void resetBufferFile()
    {
        if (!isFileOpen) {
            Serial.println("MultiOutputStream: Cannot reset - buffer file not open");
            return;
        }
        
        // Reset to empty state
        BufferFileHeader header = {0, sizeof(BufferFileHeader), sizeof(BufferFileHeader)};
        bufferFile.seek(0);
        if (bufferFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header)) {
            Serial.println("MultiOutputStream: Buffer file reset successfully");
        } else {
            Serial.println("MultiOutputStream: Failed to reset buffer file");
        }
    }
    
    size_t getBufferedMessageCount()
    {
        if (!isFileOpen) return 0;
        
        size_t originalPos = bufferFile.getPos();
        bufferFile.seek(0);
        
        BufferFileHeader header;
        size_t count = 0;
        if (bufferFile.readMemoryBlock(reinterpret_cast<char*>(&header), sizeof(header)) == sizeof(header)) {
            count = header.totalMessages;
        }
        
        bufferFile.seek(originalPos);
        return count;
    }

private:
    void handleBufferedWrite(const String& data)
    {
        size_t freeHeap = system_get_free_heap_size();
        
        // Emergency brake - refuse to do anything if memory is critically low
        if (freeHeap < 1000) {
            return; // Drop messages to prevent crash
        }
        
        // Store message in file for later delivery when memory improves
        appendMessageToFile(data);
        
        // The flush timer will handle delivery when memory allows
    }
    
    void flushBufferedData()
    {
        size_t freeHeap = system_get_free_heap_size();
        flushCounter++;
        
        // Debug timer activity every few calls
        if (flushCounter % 50 == 0) {
            //Serial.printf("MultiOutputStream: Flush timer tick %u (free: %u)\n", flushCounter, freeHeap);
        }
        
        // During low memory, be more aggressive about flushing
        if (freeHeap < 5000) {
            // Try to flush multiple messages when memory is low to clear the backlog faster
            for (int i = 0; i < 3 && freeHeap > 1500; i++) {
                loadAndProcessOneMessage();
                freeHeap = system_get_free_heap_size(); // Update after each message
                if (freeHeap < 1500) break; // Stop if memory gets too low
            }
        } else {
            // Normal operation - process one message per timer tick
            loadAndProcessOneMessage();
        }
        
        // EMERGENCY: Only disable flush if memory is extremely critically low
        if (freeHeap < 1000) {
            if (flushCounter % 20 == 0) {
                Serial.printf("MultiOutputStream: EMERGENCY - flush disabled (free: %u)\n", freeHeap);
            }
            return;
        }
    }
    
    void initializeBufferFile()
    {
        // Try to open existing file first
        if (fileExist(bufferFilePath)) {
            if (bufferFile.open(bufferFilePath, File::ReadWrite)) {
                isFileOpen = true;
                Serial.println("MultiOutputStream: Opened existing buffer file");
                return;
            }
        }
        
        // Create new file with empty header
        if (bufferFile.open(bufferFilePath, File::WriteOnly | File::Create)) {
            BufferFileHeader header = {0, sizeof(BufferFileHeader), sizeof(BufferFileHeader)};
            if (bufferFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header)) {
                // Close and reopen in ReadWrite mode
                bufferFile.close();
                if (bufferFile.open(bufferFilePath, File::ReadWrite)) {
                    isFileOpen = true;
                    Serial.println("MultiOutputStream: Created and opened new buffer file");
                    return;
                }
            }
        }
        
        isFileOpen = false;
        Serial.println("MultiOutputStream: Failed to initialize buffer file");
    }
    
    void appendMessageToFile(const String& data)
    {
        // Check if file is available
        if (!isFileOpen) {
            return; // No file available, drop message
        }
        
        size_t freeHeap = system_get_free_heap_size();
        
        // Very aggressive memory check - need substantial memory for file operations
        if (freeHeap < 4000) {
            return; // Drop messages silently when memory is critically low
        }
        
        // Strict message size limit to prevent memory exhaustion  
        if (data.length() > 128) {
            return; // Drop large messages during low memory
        }
        
        Serial.printf("MultiOutputStream: File write (free: %u, len: %u)\n", freeHeap, data.length());
        
        // Read current header
        bufferFile.seek(0);
        BufferFileHeader header;
        if (bufferFile.readMemoryBlock(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header)) {
            Serial.println("MultiOutputStream: Failed to read header");
            return;
        }
        
        // Seek to write position
        bufferFile.seek(header.writePosition);
        
        // Write message data
        size_t dataLen = data.length();
        uint8_t streamBitmap = 0; // No streams have received this message yet
        
        // Write in sequence - abort on any failure
        if (bufferFile.write(reinterpret_cast<const uint8_t*>(&dataLen), sizeof(dataLen)) != sizeof(dataLen)) {
            return;
        }
        if (bufferFile.write(reinterpret_cast<const uint8_t*>(data.c_str()), dataLen) != dataLen) {
            return;
        }
        if (bufferFile.write(reinterpret_cast<const uint8_t*>(&streamBitmap), sizeof(streamBitmap)) != sizeof(streamBitmap)) {
            return;
        }
        
        // Update header
        header.totalMessages++;
        header.writePosition = bufferFile.getPos();
        
        // Write updated header back
        bufferFile.seek(0);
        bufferFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
        
        // Only log success if we still have decent memory
        if (system_get_free_heap_size() > 3000) {
            Serial.printf("MultiOutputStream: Wrote message (total: %u)\n", header.totalMessages);
        }
        
        bufferFile.flush(); // Ensure data is written
    }
    
    void loadAndProcessOneMessage()
    {
        // Check if file is available
        if (!isFileOpen) {
            return;
        }

        // Read header
        bufferFile.seek(0);
        BufferFileHeader header;
        if (bufferFile.readMemoryBlock(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header)) {
            return;
        }

        // Check if there are messages to process
        if (header.totalMessages == 0 || header.nextReadPosition >= header.writePosition) {
            // Periodically log buffer status
            static uint32_t lastLogTime = 0;
            uint32_t now = millis();
            if (now - lastLogTime > 10000) { // Log every 10 seconds
                Serial.printf("MultiOutputStream: Buffer empty (total: %u, read: %u, write: %u, buffered streams: %u)\n", 
                             header.totalMessages, header.nextReadPosition, header.writePosition, bufferedStreams.size());
                lastLogTime = now;
            }
            return;
        }
        
        Serial.printf("MultiOutputStream: Processing message from buffer (total: %u pending)\n", header.totalMessages);
        
        Serial.printf("MultiOutputStream: Processing message from buffer (total: %u, streams: %u)\n", 
                     header.totalMessages, bufferedStreams.size());

        // Seek to next message
        bufferFile.seek(header.nextReadPosition);

        // Read one message
        size_t dataLen = 0;
        uint8_t streamBitmap = 0;
        
        if (bufferFile.readMemoryBlock(reinterpret_cast<char*>(&dataLen), sizeof(dataLen)) != sizeof(dataLen)) {
            return;
        }

        // Limit message size for safety
        if (dataLen > 512) {
            Serial.printf("MultiOutputStream: Skipping oversized message (%u bytes)\n", dataLen);
            return;
        }

        // Read message data in chunks for memory safety
        String messageData;
        messageData.reserve(dataLen);
        const size_t CHUNK_SIZE = 128; // Smaller chunks to be safer
        uint8_t readBuffer[CHUNK_SIZE];
        size_t remaining = dataLen;
        
        while (remaining > 0) {
            size_t toRead = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
            size_t bytesRead = bufferFile.readMemoryBlock(reinterpret_cast<char*>(readBuffer), toRead);
            if (bytesRead != toRead) {
                return;
            }
            messageData += String(reinterpret_cast<char*>(readBuffer), bytesRead);
            remaining -= bytesRead;
        }

        if (bufferFile.readMemoryBlock(reinterpret_cast<char*>(&streamBitmap), sizeof(streamBitmap)) != sizeof(streamBitmap)) {
            return;
        }

        // Find next stream that hasn't received this message
        bool messageProcessed = false;
        for (size_t i = 0; i < bufferedStreams.size(); i++) {
            if (!(streamBitmap & (1 << i))) { // If bit i is not set
                size_t written = bufferedStreams[i]->write(messageData.c_str(), messageData.length());
                if (written == messageData.length()) {
                    streamBitmap |= (1 << i); // Set bit i to mark stream as processed
                    messageProcessed = true;
                    Serial.printf("MultiOutputStream: Sent stored message to stream %u (%u bytes)\n", i, written);
                } else {
                    Serial.printf("MultiOutputStream: Stream %u write failed (%u/%u bytes)\n", i, written, messageData.length());
                }
                break; // Process one stream per timer tick
            }
        }

        // Check if message is complete (all streams processed)
        uint8_t allStreamsMask = (1 << bufferedStreams.size()) - 1;
        bool messageComplete = (streamBitmap & allStreamsMask) == allStreamsMask;

        // Update file header and bitmap
        if (messageComplete) {
            // Message completed - move to next message
            header.nextReadPosition = bufferFile.getPos();
            header.totalMessages--;
            Serial.printf("MultiOutputStream: Message complete, %u remaining\n", header.totalMessages);
        } else if (messageProcessed) {
            // Update bitmap for this message
            size_t currentPos = bufferFile.getPos();
            bufferFile.seek(currentPos - sizeof(streamBitmap));
            bufferFile.write(reinterpret_cast<const uint8_t*>(&streamBitmap), sizeof(streamBitmap));
        }

        // Write updated header
        bufferFile.seek(0);
        bufferFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
        bufferFile.flush(); // Ensure updates are written
    }

    std::vector<Stream*> bufferedStreams;
    std::vector<Stream*> unbufferedStreams;
    size_t minFreeHeap;
    String bufferFilePath;
    uint32_t flushIntervalMs;
    Timer flushTimer;
    uint32_t flushCounter;
    FileStream bufferFile;
    bool isFileOpen;
    bool bufferFileOpen;  // Keep buffer file open

};