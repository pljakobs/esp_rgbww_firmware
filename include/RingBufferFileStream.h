#pragma once

#include <SmingCore.h>
#include <IFS/FsBase.h>
#include <vector>
#include <string>

/*
+------------------------+
| file, size limited to  | 
| maxSize                |
|                        |
|                        |
|                        |
|                        |
|                        |
|                        |
|                        |
|                        |
|                        |
+------------------------+

I'm struggling with using the name of head and tail pointers 
as this buffer would be tail adding and reading from the head

Idea:
if x bytes are to be written, first check if maxSize-tail >x
if yes, seek to tail, write x bytes, increment tail by x
if no, write (maxSize-tail) bytes, seek to 0, write (x-(maxSize-tail)) bytes, increment tail by (x-(maxSize-tail)), set head=(tail+1)%maxSize
*/

class RingBufferFileStream : public FileStream
{
public:
    RingBufferFileStream()
        : fileName(""), maxSize(0), head(0), tail(0)
    {
    }

    RingBufferFileStream(const String& fileName, size_t maxSize)
        : fileName(fileName), maxSize(maxSize), head(0), tail(0)
    {
    }

    RingBufferFileStream(const std::string& fileName, size_t maxSize)
        : RingBufferFileStream(String(fileName.c_str()), maxSize)
    {
    }

    RingBufferFileStream(const char* fileName, size_t maxSize)
        : RingBufferFileStream(String(fileName), maxSize)
    {
    }

    bool initialize(const String& fileName, size_t maxSize)
    {
        this->fileName = fileName;
        this->maxSize = maxSize;
        return initialize();
    }

    bool initialize(const std::string& fileName, size_t maxSize)
    {
        return initialize(String(fileName.c_str()), maxSize);
    }
    
    bool initialize(const char* fileName, size_t maxSize)
    {
        return initialize(String(fileName), maxSize);
    }

    int peek() override {
        FileStream::seekFrom(readCursor, SeekOrigin::Start);
        return FileStream::peek();
    }

bool initialize()
    {
        if (FileStream::open(fileName, File::ReadWrite | File::Create))
        {
            // Initialize head and tail pointers
            head = 0;
            tail = 0;
            return true;
        }
        return false;
    }

    int seek(int offset, SeekOrigin origin) override
    {
        switch (origin)
        {
        case SeekOrigin::Start:
            readCursor = offset+head;
            break;
        case SeekOrigin::Current:
            readCursor += offset;
            break;
        case SeekOrigin::End:
            readCursor = maxSize - offset;
            break;
        }
        while (readCursor >= maxSize)
            {
                readCursor = readCursor % maxSize;
            }
        return readCursor;
    }

    int seek(int offset) override
    {
        readCursor=readCursor+offset;
        while (readCursor >= maxSize)
            {
                readCursor = readCursor % maxSize;
            }
        return readCursor;
    }

    size_t read(uint8_t* buffer, size_t size) override
    {
        size_t read,toRead = 0;
        size_t pageLeft;
        while (read < size && readCursor != tail)
        {
            if (tail>head){
                // case 1, tail is ahead of head
                // |<-head
                // |<-readCursor
                // |<-tail
                // readCursor will progress from head to tail
                pageLeft = tail - readCursor; // number of bytes left in the page
            } else {
                // case 2, tail is behind head
                // |<-tail
                // |<-head
                // |<-readCursor
                // readCursor will progress from head to maxSize, then from 0 to tail
                if (readCursor>tail){
                    //case 2a, readCursor has not yet wrapped around
                    pageLeft = maxSize - readCursor; // number of bytes left in the page
                }else {
                    //case 2b, readCursor has wrapped around
                    pageLeft = tail - readCursor; // number of bytes left in the page
                }
            }
            toRead=std::min(pageLeft, size-read);
            
            FileStream::seekFrom(readCursor, SeekOrigin::Start);
            FileStream::readBytes(reinterpret_cast<char*>(buffer + read), toRead);
            read += toRead;
            readCursor += toRead;
            
            if (readCursor >= maxSize)
                readCursor=0;
        }
        return read;
    }

    size_t write(const uint8_t* buffer, size_t size) override{
        size_t written,toWrite = 0;
        while (written < size)
        {
            if(tail>head){
                size_t pageLeft=tail-head;
            }else{
                size_t pageLeft=maxSize-head;
            }
            
            toWrite=std::min(pageLeft, size-written);

            FileStream::seekFrom(head, SeekOrigin::Start);
            FileStream::write(buffer + written, toWrite);
            written += toWrite;
            head += toWrite;

            if (tail >= maxSize)
            {
                head = 0;
                rolledOver=true;
            }

            if(rolledOver){
                tail=(head+1)%maxSize;
            }
        }
        return written;
    }

    MimeType getMimeType() override
    {
		return ContentType::fromFullFileName(getName(), MIME_TEXT);
	}


    int available() override
    {
        if(readCurser<tail){
            return tail-readCurser;
        }
        else{
            return maxSize-readCurser+tail;
        }
    }

    String readString(size_t maxLen) override
    {
        String str;
        size_t read = 0;
        while (read < maxLen && readTail != head)
        {
            FileStream::seekFrom(readTail, SeekOrigin::Start);
            char c = FileStream::read();
            if (c == '\n' || c == '\0')
            {
                break;
            }
            str += c;
            read++;
            readTail++;

            if (readTail >= maxSize)
            {
                readTail = 0;
            }
        }
        return str;
    }

private:
    String fileName;
    size_t maxSize;
    size_t head;
    size_t tail;
    
    size_t readCursor;

    bool rolledOver;
};