#pragma once

#include <Data/Stream/DataSourceStream.h>
#include <algorithm>
#include <cstring>

class BufferInputStream : public IDataSourceStream
{
public:
	explicit BufferInputStream(const String& buffer) : _data(buffer.c_str()), _length(buffer.length())
	{
	}

	StreamType getStreamType() const override
	{
		return eSST_Memory;
	}

	uint16_t readMemoryBlock(char* output, int capacity) override
	{
		if(capacity <= 0 || _position >= _length) {
			return 0;
		}
		const size_t count = std::min(static_cast<size_t>(capacity), std::min(_length - _position, size_t(UINT16_MAX)));
		std::memcpy(output, _data + _position, count);
		_position += count;
		return static_cast<uint16_t>(count);
	}

	int available() override
	{
		return static_cast<int>(_length - _position);
	}

	bool isFinished() override
	{
		return _position >= _length;
	}

private:
	const char* _data;
	size_t _length;
	size_t _position{0};
};