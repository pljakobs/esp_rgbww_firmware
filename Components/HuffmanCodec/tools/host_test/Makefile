# Host-side verification for the HuffmanCodec component.
#
#   make check CORPUS=corpus.txt [TABLE_DIR=../../include]
#
# builds the REAL firmware encoder/decoder against the table in TABLE_DIR,
# round-trips the corpus through the C++ code, then re-encodes it with
# verify_table.py (which independently parses the header) and asserts the
# compressed frames are bit-identical.
#
# Get a corpus with:
#   ../huffman_analysis.py --save-corpus corpus.txt

TABLE_DIR ?= ../../include
CORPUS    ?= corpus.txt

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror \
            -Istubs -I$(TABLE_DIR) -I../../include

SRCS := test_roundtrip.cpp \
        ../../src/HuffmanEncoder.cpp \
        ../../src/HuffmanDecoder.cpp \
        ../../src/HuffmanRingBuffer.cpp

test_roundtrip: $(SRCS) $(wildcard stubs/*.h stubs/Data/Stream/*.h) $(TABLE_DIR)/huffman_table.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

.PHONY: check clean

check: test_roundtrip
	./test_roundtrip --dump < $(CORPUS) > frames.txt
	python3 verify_table.py --table $(TABLE_DIR)/huffman_table.h \
	        --corpus $(CORPUS) --frames frames.txt

clean:
	rm -f test_roundtrip frames.txt
