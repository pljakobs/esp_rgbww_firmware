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
	: _doc(MAX_JSON_MESSAGE_LENGTH)
{
	JsonObject json = _doc.to<JsonObject>();
	json[F("jsonrpc")] = "2.0";
	json[F("method")] = name;
}

JsonObject JsonRpcMessage::getParams()
{
	if(_pParams.isNull()) {
		_pParams = _doc.as<JsonObject>().createNestedObject("params");
	}
	return _pParams;
}

JsonObject JsonRpcMessage::getRoot()
{
	return _doc.as<JsonObject>();
}

void JsonRpcMessage::setId(int id)
{
	_doc[F("id")] = id;
}



////////////////////////////////////////

JsonRpcMessageIn::JsonRpcMessageIn(const String& json)
	:_doc(MAX_JSON_MESSAGE_LENGTH)
{
	DeserializationError err = deserializeJson(_doc, json);
	if(err) {
		_valid = false;
		// Distinguish a too-small parse buffer from genuinely malformed input so
		// callers can report the real cause instead of a generic error.
		_error = (err == DeserializationError::NoMemory) ? F("message too large for parse buffer")
														 : F("malformed json");
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
