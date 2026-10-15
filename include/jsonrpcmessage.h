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

#pragma once

#include <RGBWWLed/RGBWWLed.h>
#include <JsonObjectStream.h>

#define MAX_JSON_MESSAGE_LENGTH 1024
class JsonRpcMessage {
public:
    JsonRpcMessage(const String& name);
    JsonObjectStream& getStream();
    void setId(int id);
    JsonObject getParams();
    JsonObject getRoot();
    //void setParams(String params);

private:
    JsonObjectStream _stream;
    JsonObject _pParams;
};

class JsonRpcMessageIn {
public:
    JsonRpcMessageIn(const String& json);
    JsonObject getParams();

    JsonObject getRoot();
    String getMethod();
    bool isValid() const { return _valid; }
    const String& getError() const { return _error; }

private:
    DynamicJsonDocument _doc;
    bool _valid{false};
    String _error;
};

