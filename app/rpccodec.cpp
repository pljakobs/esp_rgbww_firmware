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

RpcCodec::RpcCodec() : _db(F("jsonrpc"))
{
	// Protocol messages are transient. Store::commit() re-checks the dirty flag
	// after this callback, so clearing it here suppresses the flash write.
	Jsonrpc::Root::onCommit(_db, [](Jsonrpc::RootUpdater root) { root.clearDirty(); });
}

bool RpcCodec::render(const Message& msg, const ConfigDB::Object& body, String& out)
{
	MemoryDataStream mem;
	if(!JsonRPC::exportMessage(msg, body, mem)) {
		return false;
	}

	return mem.moveString(out);
}

bool RpcCodec::renderPayload(const ConfigDB::Object& body, String& out)
{
	if(!body) {
		return false;
	}

	MemoryDataStream mem;
	ConfigDB::ExportOptions options;
	options.asObject = true;
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
