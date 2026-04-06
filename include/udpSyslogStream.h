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
 *
 * @section DESCRIPTION
 *
 *
 */

#pragma once
#include <SmingCore.h>
#include <Network/UdpConnection.h>
#include <HuffmanRingBuffer.h>
#include <HuffmanEncoder.h>
#include <HuffmanDecoder.h>
#include <memory>

/**
 * UdpSyslogStream — RFC 3164 UDP syslog stream.
 *
 * Uses a double-buffer scheme: incoming characters fill the active buffer.
 * When a newline or capacity limit (MAX_MSG_LEN) is hit, the active buffer is
 * swapped out and its content sent as a UDP packet, while the other buffer
 * immediately accepts new characters. This ensures no data is lost while a
 * packet is being handed to lwIP.
 *
 * Format: "<priority>hostname tag: message\n"
 * Priority = facility*8 + severity (default: LOCAL0 | DEBUG = 184 | 7 = 191)
 *
 * Before begin() is called, all writes are compressed (Huffman + n-gram tokens)
 * into a heap-allocated 2 KB ring buffer.  On begin(), the ring buffer is drained:
 * each compressed frame is decoded and sent as a normal syslog packet, then the
 * ring buffer and codec are freed — reclaiming ~3 KB of heap for normal operation.
 * This preserves boot-time log messages that arrive before the network is ready.
 *
 * huffman_table.h (PROGMEM encode/decode tables) must be pre-generated:
 *   python3 huffman_analysis.py --ngrams --emit --out huffman_table.h
 */
class UdpSyslogStream : public Stream
{
public:
    static const uint16_t SYSLOG_PORT = 514;
    // Max message payload — leaves ~40 bytes headroom for RFC 3164 header within 512-byte UDP
    static const size_t MAX_MSG_LEN = 470;

    UdpSyslogStream()
    {
        _buf[0].reserve(MAX_MSG_LEN);
        _buf[1].reserve(MAX_MSG_LEN);
        _active = 0;
        _pending[0] = false;
        _pending[1] = false;

        // Allocate pre-network codec on the heap.
        // If allocation fails we simply won't buffer pre-network messages.
        auto* mem = new(std::nothrow) uint8_t[PRE_NET_BUF_SIZE];
        if(mem) {
            _ringMem.reset(mem);
            _preNetBuf.reset(new(std::nothrow) HuffmanRingBuffer(mem, PRE_NET_BUF_SIZE));
            if(_preNetBuf) {
                _encoder.reset(new(std::nothrow) HuffmanEncoder(*_preNetBuf));
            }
        }
    }

    /**
     * @param host      Syslog collector IP address string (e.g. "192.168.1.10")
     * @param port      UDP port (default 514)
     * @param hostname  Reported hostname in the syslog packet
     * @param tag       Reported process/app tag
     * @param priority  RFC 3164 PRI value (default 191 = LOCAL0|DEBUG)
     */
    void begin(const String& host, uint16_t port = SYSLOG_PORT,
               const String& hostname = "rgbww",
               const String& tag = "app",
               uint8_t priority = 191,
               bool enabled = true)
    {
        _host     = host;
        _port     = port;
        _hostname = hostname;
        _tag      = tag;
        _priority = priority;
        _enabled  = enabled;
        _udp.connect(IpAddress(_host), _port);
        _ready    = true;

        // Drain pre-network ring buffer: decode each compressed frame and
        // send it through the normal UDP syslog path.
        if(_preNetBuf) {
            uint8_t  frame[HuffmanEncoder::OUTPUT_BUF_SIZE];
            char     msg[MAX_MSG_LEN + 1];
            uint16_t frameLen, msgLen;
            while(_preNetBuf->read(frame, sizeof(frame), frameLen)) {
                msgLen = HuffmanDecoder::decodeFrame(frame, frameLen, msg, MAX_MSG_LEN);
                for(uint16_t i = 0; i < msgLen; i++) {
                    if(_buf[_active].length() < MAX_MSG_LEN) {
                        _buf[_active] += msg[i];
                    }
                }
                if(_buf[_active].length() > 0) {
                    queueActiveBuf();
                    flushPending();
                }
            }

            // Reclaim ~3 KB: ring buffer backing store + encoder buffers
            _encoder.reset();
            _preNetBuf.reset();
            _ringMem.reset();
        }

        // Flush anything left in the double-buffer (normally empty at this point).
        if(_buf[_active].length() > 0) {
            queueActiveBuf();
        }
        flushPending();
    }

    void end()
    {
        flush();
        _enabled = false;
        _udp.close();
    }

    // --- Stream interface ---

    IRAM_ATTR size_t write(uint8_t c) override
    {
        // Before begin(): compress into pre-network ring buffer regardless of
        // _enabled — we don't know the final rsyslog config yet.
        if(!_ready) {
            if(c != '\r' && _encoder) {
                if(c == '\n') {
                    _encoder->flush();
                } else {
                    _encoder->write(c);
                }
            }
            return 1;
        }
        if(!_enabled) {
            return 1; // rsyslog disabled — drop
        }
        if(c == '\r') {
            return 1;
        }
        if(c == '\n') {
            queueActiveBuf();
            flushPending();
            return 1;
        }
        if(_buf[_active].length() < MAX_MSG_LEN) {
            _buf[_active] += (char)c;
        }
        if(_buf[_active].length() >= MAX_MSG_LEN) {
            queueActiveBuf();
            flushPending();
        }
        return 1;
    }

    IRAM_ATTR size_t write(const uint8_t* buffer, size_t size) override
    {
        for(size_t i = 0; i < size; i++) {
            write(buffer[i]);
        }
        return size;
    }

    int available() override { return 0; }
    int read()      override { return -1; }
    int peek()      override { return -1; }

    void flush() override
    {
        if (_buf[_active].length() > 0) {
            queueActiveBuf();
        }
        if (_enabled) {
            flushPending();
        }
    }

    void enable() { _enabled = true; }
    void disable() { _enabled = false; }
    void setStatus(bool status) {_enabled=status;}
    bool isEnabled() { return _enabled; }
private:
    /**
     * Append a completed buffer into the pending ring queue.
     * Queue capacity is 2; oldest pending buffer is dropped when full.
     */
    void queuePendingIndex(int idx)
    {
        if (_pendingCount == 2) {
            int drop = _pendingOrder[_pendingHead];
            _pending[drop] = false;
            _pendingHead = (_pendingHead + 1) % 2;
            _pendingCount--;
        }

        int tail = (_pendingHead + _pendingCount) % 2;
        _pendingOrder[tail] = idx;
        _pending[idx] = true;
        _pendingCount++;
    }

    /**
     * Move active buffer into pending queue and switch to the other buffer.
     */
    void queueActiveBuf()
    {
        if (_buf[_active].length() == 0) {
            return;
        }

        int sendBuf = _active;
        _active = 1 - _active;
        _buf[_active] = "";
        queuePendingIndex(sendBuf);
    }

    /**
     * Send all queued buffers in FIFO order (normal streaming mode).
     */
    void flushPending()
    {
        while (_enabled && _pendingCount > 0) {
            int sendBuf = _pendingOrder[_pendingHead];
            _pendingHead = (_pendingHead + 1) % 2;
            _pendingCount--;
            _pending[sendBuf] = false;

            // Build RFC 3164 packet in-place to avoid extra allocation.
            String& msg = _buf[sendBuf];

            String hdr;
            hdr.reserve(20 + _hostname.length() + _tag.length());
            hdr += '<';
            hdr += String(_priority);
            hdr += '>';
            hdr += _hostname;
            hdr += ' ';
            hdr += _tag;
            hdr += ": ";

            msg = hdr + msg + '\n';

            _udp.sendTo(IpAddress(_host), _port, msg.c_str(), msg.length());

            _buf[sendBuf] = "";
        }
    }

    UdpConnection _udp;
    String        _host;
    uint16_t      _port{SYSLOG_PORT};
    String        _hostname{"rgbww"};
    String        _tag{"app"};
    uint8_t       _priority{191};
    bool          _enabled{false};
    bool          _ready{false};   ///< true once begin() has been called

    String _buf[2];
    int    _active{0};
    bool   _pending[2]{false, false};
    int    _pendingOrder[2]{0, 1};
    int    _pendingHead{0};
    int    _pendingCount{0};

    // Pre-network compressed ring buffer — heap-allocated before begin(),
    // freed after drain so the ~3 KB is returned to the heap.
    static constexpr uint16_t PRE_NET_BUF_SIZE = 2048;
    std::unique_ptr<uint8_t[]>        _ringMem;
    std::unique_ptr<HuffmanRingBuffer> _preNetBuf;
    std::unique_ptr<HuffmanEncoder>    _encoder;
};
