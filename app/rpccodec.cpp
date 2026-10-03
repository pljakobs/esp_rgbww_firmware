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
#include <Data/Stream/MemoryDataStream.h>

namespace
{
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
