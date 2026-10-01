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

#include <RGBWWLed/RGBWWLedColor.h>
#include <rpccodec.h>
#include <vector>


/**
 * @class JsonProcessor
 * @brief A class for processing JSON commands related to color control.
 *
 * The JsonProcessor class provides methods for handling various color control commands
 * received in JSON format. It includes functions for setting color, stopping, skipping,
 * pausing, continuing, blinking, toggling, and executing direct commands. It also supports
 * JSON-RPC commands.
 */

class JsonProcessor {
public:
    struct RequestParameters {
        String target;

        enum class Mode {
            Undefined,
            Hsv,
            Raw,
            Kelvin,
        };

        Mode mode = Mode::Undefined;

        bool hasHsvFrom = false;
        bool hasRawFrom = false;

        RequestHSVCT hsv;
        RequestHSVCT hsvFrom;

        RequestChannelOutput raw;
        RequestChannelOutput rawFrom;

        int direction = 1;
        bool requeue = false;
        RampTimeOrSpeed ramp = 0;
        String name;

        String cmd = "solid";

        RGBWWLed::ChannelList channels;

        QueuePolicy queue = QueuePolicy::Single;

        int checkParams(String& errorMsg) const;
    };

    bool onColor(JsonObject root, String& msg, bool relay = true);
    bool onStop(JsonObject root, String& msg, bool relay = true);
    bool onSkip(JsonObject root, String& msg, bool relay = true);
    bool onPause(JsonObject root, String& msg, bool relay = true);
    bool onContinue(JsonObject root, String& msg, bool relay = true);
    bool onBlink(JsonObject root, String& msg, bool relay = true);
    bool onSetOn(JsonObject root, String& msg, bool relay = true);
    bool onSetOff(JsonObject root, String& msg, bool relay = true);
    bool onToggle(JsonObject root, String& msg, bool relay = true);
    bool onDirect(JsonObject root, String& msg, bool relay);

    bool onJsonRpc(const String& json);

    void parseRequestParams(JsonObject root, RequestParameters& params);

    /**
     * @brief Import a command body into the transient jsonrpc store and convert it to RequestParameters.
     * @param batch If non-null, receives one entry per "cmds" item
     * @note The store is released before returning, so callers can execute commands
     * (which may render through rpcCodec()) without clobbering it.
     */
    bool parseRequest(Stream& body, RequestParameters& params, std::vector<RequestParameters>* batch,
                      String& errorMsg);

    void addChannelStatesToCmd(JsonObject root, const RGBWWLed::ChannelList& channels);

    // Transport-agnostic command execution, shared by the JsonObject and ConfigDB paths
    bool runColor(RequestParameters& params, std::vector<RequestParameters>& batch, String& errorMsg);
    void runStop(const RequestParameters& params, String& msg);
    void runSkip(const RequestParameters& params, String& msg);
    void runPause(const RequestParameters& params, String& msg);
    void runContinue(const RequestParameters& params);
    void runBlink(const RequestParameters& params);
    void runToggle();
    void runDirect(const RequestParameters& params, String& msg);
    void runSetOn(const RequestParameters& params);
    void runSetOff(RequestParameters& params);

private:
    bool runColorCommand(RequestParameters& params, String& errorMsg);
};
