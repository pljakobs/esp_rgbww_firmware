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

#include <RGBWWCtrl.h>
#include <ConfigDB/JsonRPC/ReadStream.h>
#include <Data/Stream/MemoryDataStream.h>
#include <Network/Http/Websocket/WebsocketConnection.h>
#include <Platform/System.h>
#include <array>
#include <cstdlib>
#include <deque>
#include <functional>

namespace
{
constexpr ws_frame_type_t WS_FRAME_CONTINUATION_OPCODE = static_cast<ws_frame_type_t>(0x0);

// Flips `flag` back off on scope exit, regardless of which return statement is taken.
class ScopedFlag
{
public:
	explicit ScopedFlag(bool& flag) : flag(flag)
	{
		flag = true;
	}
	~ScopedFlag()
	{
		flag = false;
	}

private:
	bool& flag;
};
} // namespace

struct RpcCodec::StreamTask::State : std::enable_shared_from_this<State>
{
	static constexpr size_t SegmentStep = 256;
	static constexpr size_t MaxSegmentSize = 1024;
	static constexpr size_t RestrictedSegmentSize = 512;
	static constexpr size_t RestrictedHeapThreshold = 8192;
	static constexpr size_t HeapReserve = 4096;
	static constexpr size_t AllocationMargin = 64;
	static constexpr size_t BufferCount = 3;

	struct Slot {
		~Slot()
		{
			free(data);
		}

		char* data{};
		size_t capacity{};
		size_t length{};
		bool used{};
		bool inFlight{};
	};

	class SegmentStream : public IDataSourceStream
	{
	public:
		SegmentStream(std::shared_ptr<State> owner, size_t index) : owner(std::move(owner)), index(index)
		{
		}

		~SegmentStream() override
		{
			owner->release(index);
		}

		StreamType getStreamType() const override
		{
			return eSST_Wrapper;
		}

		int available() override
		{
			return static_cast<int>(slot().length - position);
		}

		uint16_t readMemoryBlock(char* data, int size) override
		{
			if(data == nullptr || size <= 0) {
				return 0;
			}
			const size_t count = std::min<size_t>(size, slot().length - position);
			memcpy(data, slot().data + position, count);
			return static_cast<uint16_t>(count);
		}

		bool seek(int offset) override
		{
			if(offset < 0 || static_cast<size_t>(offset) > slot().length - position) {
				return false;
			}
			position += static_cast<size_t>(offset);
			return true;
		}

		bool isFinished() override
		{
			return position == slot().length;
		}

	private:
		Slot& slot()
		{
			return owner->slots[index];
		}

		std::shared_ptr<State> owner;
		const size_t index;
		size_t position{};
	};

	State(WebsocketConnection& socket, std::unique_ptr<IDataSourceStream> source, std::function<void(bool)> onComplete)
		: socket(&socket), source(std::move(source)), onComplete(std::move(onComplete))
	{
	}

	void start()
	{
		pump();
	}

	void cancel()
	{
		active = false;
		source.reset();
		onComplete = nullptr;
	}

	void release(size_t index)
	{
		auto& slot = slots[index];
		if(slot.inFlight) {
			slot.inFlight = false;
			--inFlight;
		}
		slot.used = false;
		slot.length = 0;
		free(slot.data);
		slot.data = nullptr;
		slot.capacity = 0;
		if(active) {
			schedulePump();
		}
	}

private:
	void schedulePump()
	{
		if(pumpQueued || !active) {
			return;
		}
		pumpQueued = true;
		auto self = shared_from_this();
		if(!System.queueCallback([self]() {
			   self->pumpQueued = false;
			   self->pump();
		   })) {
			pumpQueued = false;
			finish(false);
		}
	}

	bool growSlot(Slot& slot)
	{
		const size_t freeHeap = app.getFreeHeapSize();
		if(freeHeap < RestrictedHeapThreshold) {
			segmentLimit = RestrictedSegmentSize;
		}
		const size_t capacity = slot.capacity + SegmentStep;
		if(capacity > segmentLimit || freeHeap < HeapReserve + capacity + AllocationMargin) {
			return false;
		}
		auto* data = static_cast<char*>(realloc(slot.data, capacity));
		if(data == nullptr) {
			segmentLimit = RestrictedSegmentSize;
			return false;
		}
		slot.data = data;
		slot.capacity = capacity;
		return true;
	}

	bool fillSlot(size_t index)
	{
		auto& slot = slots[index];
		slot.length = 0;
		if(!growSlot(slot)) {
			return false;
		}
		while(true) {
			if(slot.length == slot.capacity && !growSlot(slot)) {
				return true;
			}
			const auto count = source->readMemoryBlock(slot.data + slot.length,
													 static_cast<int>(slot.capacity - slot.length));
			if(count == 0) {
				if(source->isFinished()) {
					eof = true;
					return true;
				}
				debug_e("RpcCodec: source stalled while filling a WebSocket segment");
				return false;
			}
			if(!source->seek(count)) {
				debug_e("RpcCodec: source seek failed while filling a WebSocket segment");
				return false;
			}
			slot.length += count;
		}
	}

	void pump()
	{
		if(!active || pumping) {
			return;
		}
		pumping = true;

		for(size_t i = 0; i < slots.size() && !eof; ++i) {
			auto& slot = slots[i];
			if(slot.used) {
				continue;
			}
			if(!fillSlot(i)) {
				pumping = false;
				finish(false);
				return;
			}
			if(slot.length != 0) {
				slot.used = true;
				ready.push_back(i);
			}
		}

		while(!ready.empty() && (ready.size() > 1 || eof)) {
			const size_t index = ready.front();
			ready.pop_front();
			auto& slot = slots[index];
			const bool fin = eof && ready.empty();
			const auto type = firstFrame ? WS_FRAME_TEXT : WS_FRAME_CONTINUATION_OPCODE;
			if(app.getFreeHeapSize() < HeapReserve + sizeof(SegmentStream) + AllocationMargin) {
				pumping = false;
				finish(false);
				return;
			}
			auto frame = new SegmentStream(shared_from_this(), index);
			if(frame == nullptr) {
				pumping = false;
				finish(false);
				return;
			}
			slot.inFlight = true;
			++inFlight;
			firstFrame = false;
			if(!socket->send(frame, type, false, fin)) {
				pumping = false;
				finish(false);
				return;
			}
		}

		pumping = false;
		if(eof && ready.empty() && inFlight == 0) {
			finish(true);
		}
	}

	void finish(bool success)
	{
		if(!active) {
			return;
		}
		active = false;
		source.reset();
		auto callback = std::move(onComplete);
		if(callback) {
			if(!System.queueCallback([callback = std::move(callback), success]() { callback(success); })) {
				debug_e("RpcCodec: unable to queue WebSocket completion callback");
				socket->close();
			}
		}
	}

	WebsocketConnection* socket;
	std::unique_ptr<IDataSourceStream> source;
	std::function<void(bool)> onComplete;
	std::array<Slot, BufferCount> slots{};
	std::deque<size_t> ready;
	size_t segmentLimit{MaxSegmentSize};
	size_t inFlight{};
	bool active{true};
	bool eof{};
	bool firstFrame{true};
	bool pumping{};
	bool pumpQueued{};
};

RpcCodec::StreamTask::~StreamTask()
{
	cancel();
}

void RpcCodec::StreamTask::cancel()
{
	if(state) {
		state->cancel();
		state.reset();
	}
}

RpcCodec::RpcCodec() : _db(F("jsonrpc"))
{
	// Protocol messages are transient. Store::commit() re-checks the dirty flag
	// after this callback, so clearing it here suppresses the flash write.
	Jsonrpc::Root::onCommit(_db, [](Jsonrpc::RootUpdater root) { root.clearDirty(); });
}

bool RpcCodec::render(const Message& msg, const ConfigDB::Object& body, String& out)
{
	if(_rendering) {
		cdebug_e(RPCCODEC, "RpcCodec::render: " ANSI_COLOR_RED "re-entrant call while another message is serializing - dropping to avoid corrupting the shared jsonrpc store" ANSI_COLOR_RESET);
		return false;
	}
	ScopedFlag guard(_rendering);

	MemoryDataStream mem;
	if(!JsonRPC::exportMessage(msg, body, mem)) {
		return false;
	}

	return mem.moveString(out);
}

std::unique_ptr<IDataSourceStream> RpcCodec::renderStream(const Message& msg, const ConfigDB::Object& body)
{
	if(_rendering || !msg || !body) {
		return nullptr;
	}
	ScopedFlag guard(_rendering);

	ConfigDB::ObjectRef bodyRef(body);
	if(!bodyRef) {
		return nullptr;
	}

	return std::unique_ptr<IDataSourceStream>(new JsonRPC::ReadStream(msg, bodyRef));
}

std::shared_ptr<RpcCodec::StreamTask> RpcCodec::sendWebsocketStream(WebsocketConnection& socket,
																		 std::unique_ptr<IDataSourceStream> source,
																		 std::function<void(bool)> onComplete)
{
	if(!source) {
		return nullptr;
	}
	if(app.getFreeHeapSize() < StreamTask::State::HeapReserve + sizeof(StreamTask::State)
								 + sizeof(StreamTask) + 2 * StreamTask::State::AllocationMargin) {
		return nullptr;
	}
	auto state = std::shared_ptr<StreamTask::State>(
		new StreamTask::State(socket, std::move(source), std::move(onComplete)));
	if(!state) {
		return nullptr;
	}
	auto task = std::shared_ptr<StreamTask>(new StreamTask(state));
	if(!task) {
		state->cancel();
		return nullptr;
	}
	state->start();
	return task;
}

bool RpcCodec::renderPayload(const ConfigDB::Object& body, String& out)
{
	if(_rendering) {
		cdebug_e(RPCCODEC, "RpcCodec::renderPayload: " ANSI_COLOR_RED "re-entrant call while another message is serializing - dropping to avoid corrupting the shared jsonrpc store" ANSI_COLOR_RESET);
		return false;
	}
	ScopedFlag guard(_rendering);

	if(!body) {
		return false;
	}

	MemoryDataStream mem;
	ConfigDB::ExportOptions options;
	// A bare payload must contain only the object's properties. ConfigDB's
	// asObject style also emits the root object's schema name, which would turn
	// e.g. {"scanning":...} into {"networks-params":{...}}.
	options.asObject = false;
	if(ConfigDB::Json::format.exportToStream(body, mem, options) == 0) {
		return false;
	}

	return mem.moveString(out);
}

RpcCodec& rpcCodec()
{
	static RpcCodec codec;
	return codec;
}
