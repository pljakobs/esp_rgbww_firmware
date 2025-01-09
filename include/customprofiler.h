class CustomProfiler : public IFS::IProfiler
{
public:
	struct Stat {
		size_t blockSize;
		size_t blockCount;
		std::unique_ptr<size_t[]> count;

		Stat(Storage::Partition part)
			: blockSize(part.getBlockSize()), blockCount(part.size() / blockSize),
			  count(std::make_unique<size_t[]>(blockCount))
		{
		}

		void reset()
		{
			std::fill_n(count.get(), blockCount, 0);
		}

		void update(storage_size_t address, size_t)
		{
			auto blockNumber = address / blockSize;
			assert(blockNumber < blockCount);
			++count[blockNumber];
		}

		size_t printTo(Print& p) const
		{
			size_t n{0};
			for(unsigned i = 0; i < blockCount; ++i) {
				if(i % 8 == 0) {
					n += p.println();
					n += p.print(String(i).padLeft(3));
					n += p.print(':');
				}
				n += p.print(' ');
				n += p.print(String(count[i]).padLeft(7));
			}
			return n;
		}
	};

	Stat readStat;
	Stat writeStat;
	Stat eraseStat;

	CustomProfiler(Storage::Partition part) : readStat(part), writeStat(part), eraseStat(part)
	{
	}

	void read(storage_size_t address, const void* buffer, size_t size) override
	{
		(void)buffer;
		readStat.update(address, size);
	}

	void write(storage_size_t address, const void* buffer, size_t size) override
	{
		(void)buffer;
		writeStat.update(address, size);
	}

	void erase(storage_size_t address, size_t size) override
	{
		eraseStat.update(address, size);
	}

	void reset()
	{
		readStat.reset();
		writeStat.reset();
		eraseStat.reset();
	}

	size_t printTo(Print& p) const
	{
		size_t n{0};
		//n += p.print(_F("Read: "));
		//n += p.println(readStat);
		n += p.print(_F("Write: "));
		n += p.println(writeStat);
		//n += p.print(_F("Erase: "));
		//n += p.println(eraseStat);
		return n;
	}
};