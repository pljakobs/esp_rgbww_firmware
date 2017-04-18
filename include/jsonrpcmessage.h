#pragma once

#include <SmingCore/SmingCore.h>

#include <RGBWWLed/RGBWWLed.h>


class JsonRpcMessage {
public:
    JsonRpcMessage(const String& name);
    JsonObjectStream& getStream();
    void setId(int id);
    JsonObject& getParams();
    JsonObject& getRoot();

private:
    const String _data;

    JsonObjectStream _stream;
    JsonObject* _pParams;
};

class JsonRpcMessageIn {
public:
	JsonRpcMessageIn(const String& json);
    JsonObject& getParams();

    JsonObject& getRoot();
    String getMethod();

private:
    JsonObject* _root = nullptr;
    DynamicJsonBuffer _jsonBuffer;
};

