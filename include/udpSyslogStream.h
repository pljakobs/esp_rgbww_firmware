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
 * Before begin() is called, all writes are Huffman-compressed into a heap-allocated
 * ring buffer.  On drainPreNetBuffer() (called from the GotIP callback once the UDP
 * route exists) each compressed frame is decoded and sent as a normal syslog packet,
 * then the ring buffer and codec are freed, reclaiming heap for normal operation.
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

        // Allocate pre-network Huffman codec on the heap.
        auto* mem = new uint8_t[PRE_NET_BUF_SIZE];
        _ringMem.reset(mem);
        _preNetBuf.reset(new HuffmanRingBuffer(mem, PRE_NET_BUF_SIZE));
        _encoder.reset(new HuffmanEncoder(*_preNetBuf));
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
        // Do NOT set _ready = true here: begin() is called before the network has an
        // IP address, so any write() that goes through flushPending() → sendTo() would
        // be silently dropped.  Keep routing to the ring buffer until drainPreNetBuffer()
        // is called from the GotIP callback, at which point the route actually exists.
    }

    /**
     * Replay all messages stored in the pre-network ring buffer through the normal
     * UDP syslog path and then free the buffer.  Must be called after the station
     * has obtained an IP address so that UDP packets can actually be routed.
     * Safe to call multiple times; does nothing if the buffer has already been freed.
     */
    void drainPreNetBuffer()
    {
        if(!_preNetBuf) {
            return;
        }

        // Keep _ready = false: all app writes during the drain still go into the
        // ring buffer so they are delivered in order.  _ready is set true when the
        // ring buffer is exhausted at the end of _drainStep().
        // _draining = true lets write() know that drain-internal replays (guarded
        // by _replayingFrame) must bypass the ring buffer path and go to UDP.
        _draining = true;

        debug_i("drainPreNetBuffer: %u messages, %u/%u bytes used, %u evicted",
                _preNetBuf->count(), _preNetBuf->used(), _preNetBuf->capacity(),
                _preNetBuf->evictedCount());
        // Capture a random nonce; the sentinel itself is sent at the start of
        // the first _drainStep() tick rather than here.  Sending sendTo() directly
        // in the GotIP callback context triggers the same silent packet loss as
        // write() — the lwIP stack is mid-transition.
        _sentinelNonce = static_cast<uint32_t>(os_random());
        _needsSentinel = true;
        _drainEvicted  = _preNetBuf->evictedCount();
        // Pace drain: fire first _drainStep() after one timer interval so lwIP
        // is fully ready, then keep 10 ms between every subsequent packet.
        // queueCallback (µs apart) exhausts the pbuf pool and causes ~30% loss.
        _drainTimer.initializeMs(DRAIN_INTERVAL_MS, [this]() { _drainStep(); }).startOnce();
    }

    void end()
    {
        flush();
        _enabled = false;
        _udp.close();
    }

private:
    /**
     * Send one decoded frame over UDP and re-queue itself until the ring buffer
     * is exhausted.  Called via System.queueCallback so the OS (and lwIP TX
     * queue) gets a chance to run between every packet, preventing pbuf pool
     * exhaustion that silently drops all but the first few frames.
     */
    void _drainStep()
    {
        if(!_preNetBuf) {
            return;
        }

        // Send restart sentinel on the first OS tick after GotIP, when the
        // network stack is fully ready.  sendTo() in the GotIP callback itself
        // fires while lwIP is mid-transition and silently drops the packets.
        if(_needsSentinel) {
            _needsSentinel = false;
            String pkt;
            pkt.reserve(80);
            pkt += '<';
            pkt += String(_priority);
            pkt += '>';
            pkt += _hostname.length() ? _hostname : F("device");
            pkt += ' ';
            pkt += _tag;
            // Include nonce in header position too so the per-line nonce parser
            // on the server can detect the boot transition from the sentinel packet.
            pkt += F(": nonce:");
            pkt += String(_sentinelNonce);
            pkt += F(" 0 ===== system restart ===== nonce:");
            pkt += String(_sentinelNonce);
            pkt += '\n';
            for(int repeat = 0; repeat < 3; ++repeat) {
                _udp.sendTo(IpAddress(_host), _port, pkt.c_str(), pkt.length());
            }
        }

        uint8_t  frame[HuffmanEncoder::OUTPUT_BUF_SIZE];
        char     msg[MAX_MSG_LEN + 1];
        uint16_t frameLen, msgLen;
        if(!_preNetBuf->read(frame, sizeof(frame), frameLen)) {
            // Ring buffer exhausted — drain complete.  From now on writes go
            // directly to UDP via the normal path.
            _encoder.reset();
            _preNetBuf.reset();
            _ringMem.reset();
            _draining = false;
            _ready    = true;
            if(_buf[_active].length() > 0) {
                queueActiveBuf();
            }
            // Emit eviction summary as a real syslog line so it shows in the log viewer.
            if(_drainEvicted > 0) {
                String ev;
                ev += F("pre-net buffer: ");
                ev += String(_drainEvicted);
                ev += F(" message(s) evicted due to buffer wraparound");
                println(ev);
            }
            flushPending();
            return;
        }

        msgLen = HuffmanDecoder::decodeFrame(frame, frameLen, msg, MAX_MSG_LEN);
        // _replayingFrame = true makes write() bypass the ring buffer path so
        // these decoded bytes go to UDP instead of looping back into the encoder.
        _replayingFrame = true;
        for(uint16_t i = 0; i < msgLen; i++) {
            write((uint8_t)msg[i]);
        }
        _replayingFrame = false;
        if(_buf[_active].length() > 0) {
            queueActiveBuf();
            flushPending();
        }
        // Pace: wait DRAIN_INTERVAL_MS before sending next frame so lwIP can
        // fully transmit the previous packet and avoid pbuf pool exhaustion.
        _drainTimer.startOnce();
    }

public:

    IRAM_ATTR size_t write(uint8_t c) override
    {
        // Pre-GotIP or app write during drain: compress into the ring buffer so
        // messages are delivered in chronological order.  _replayingFrame gates
        // the exception: when _drainStep() replays a decoded frame it must bypass
        // this path (or the bytes would loop back into the encoder).
        if(!_ready && !_replayingFrame) {
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

    /**
     * @brief Pre-network buffer status for external monitoring.
     *
     * Returns PreNetState::Buffering while writing into the ring buffer before
     * the network is up, PreNetState::Draining while replaying buffered messages
     * to UDP, and PreNetState::Done once the buffer has been freed and direct UDP
     * routing is active.
     */
    enum class PreNetState : uint8_t { Buffering, Draining, Done };
    PreNetState preNetState() const
    {
        if(!_preNetBuf) return PreNetState::Done;
        if(_draining)   return PreNetState::Draining;
        return PreNetState::Buffering;
    }

    void enable() { _enabled = true; }
    void disable() { _enabled = false; }
    void setStatus(bool status) {_enabled=status;}
    bool isEnabled() { return _enabled; }
    void setHostname(const String& hostname) { _hostname = hostname; }
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
            // Embed the boot nonce in every packet so the log service can
            // assign lines to the correct boot without waiting for the sentinel.
            if (_sentinelNonce != 0) {
                hdr += F("nonce:");
                hdr += String(_sentinelNonce);
                hdr += ' ';
            }

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
    bool          _ready{false};          ///< true after drain completes; direct UDP path active
    bool          _draining{false};       ///< true while pre-net buffer is being drained
    bool          _replayingFrame{false}; ///< true inside _drainStep() replay loop — bypasses ring buffer in write()
    bool          _needsSentinel{false};  ///< send restart sentinel at start of next _drainStep()
    uint32_t      _sentinelNonce{0};      ///< nonce to stamp on the sentinel packet
    uint16_t      _drainEvicted{0};       ///< evicted count captured at drain start, for syslog emit

    /// Inter-packet delay during pre-net buffer drain (ms).
    /// 10 ms gives lwIP time to hand each UDP packet to the WiFi chip before the
    /// next one arrives, preventing pbuf pool exhaustion and the ~30% silent loss
    /// seen when using queueCallback (µs-apart).
    static constexpr uint32_t DRAIN_INTERVAL_MS = 10;
    Timer        _drainTimer; ///< paces _drainStep() calls during pre-net buffer replay

    String _buf[2];
    int    _active{0};
    bool   _pending[2]{false, false};
    int    _pendingOrder[2]{0, 1};
    int    _pendingHead{0};
    int    _pendingCount{0};

    // Pre-network Huffman ring buffer — heap-allocated before begin(),
    // freed after drainPreNetBuffer() so the ~6 KB is returned to the heap.
    static constexpr uint16_t PRE_NET_BUF_SIZE = 6144;
    std::unique_ptr<uint8_t[]>         _ringMem;
    std::unique_ptr<HuffmanRingBuffer> _preNetBuf;
    std::unique_ptr<HuffmanEncoder>    _encoder;
};
