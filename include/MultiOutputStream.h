#include <SmingCore.h>
#include <vector>
#include <algorithm>

class MultiOutputStream : public Stream
{
public:
    void addStream(Stream* stream)
    {
        streams.push_back(stream);
    }

    void removeStream(Stream* stream)
    {
        streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
    }

    size_t write(uint8_t c) override
    {
        for (auto stream : streams)
        {
            stream->write(c);
        }
        return 1;
    }

    size_t write(const uint8_t* buffer, size_t size) override
    {
        for (auto stream : streams)
        {
            stream->write("*"); // For verification
            stream->write(buffer, size);
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
        for (auto stream : streams)
        {
            stream->flush();
        }
    }

private:
    std::vector<Stream*> streams;
};