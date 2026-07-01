/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 *          Peter Jakobs http://github.com/pljakobs
 * 
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
#include "jsonrpcmessage.h"

JsonRpcMessage::JsonRpcMessage(const String& name)
{
	JsonObject json = _stream.getRoot();
	json[F("jsonrpc")] = "2.0";
	json[F("method")] = name;
}

JsonObjectStream& JsonRpcMessage::getStream()
{
	return _stream;
}

JsonObject JsonRpcMessage::getParams()
{
	if(_pParams.isNull()) {
		_pParams = _stream.getRoot().createNestedObject("params");
	}
	return _pParams;
}

JsonObject JsonRpcMessage::getRoot()
{
	return _stream.getRoot();
}

void JsonRpcMessage::setId(int id)
{
	JsonObject json = _stream.getRoot();
	json[F("id")] = id;
}



////////////////////////////////////////

JsonRpcMessageIn::JsonRpcMessageIn(const String& json)
{
	const bool parsed = Json::deserialize(_doc, json);
	if(!parsed) {
		_valid = false;
		// Avoid allocating error string for malformed input
		// getError() will return empty string or default if needed
		return;
	}

	_valid = true;
}

JsonObject JsonRpcMessageIn::getParams()
{
	return _doc[F("params")];
}

JsonObject JsonRpcMessageIn::getRoot()
{
	return _doc.as<JsonObject>();
}

const char* JsonRpcMessageIn::getMethod() const
{
	const char* method = _doc[F("method")] | "";
	return method;
}
