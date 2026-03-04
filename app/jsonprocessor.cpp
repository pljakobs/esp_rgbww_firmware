
#include <RGBWWCtrl.h>
#include <application.h>

#define MIN_HEAP_FREE 8192
/**
 * @brief Processes the color JSON data.
 *
 * This function deserializes the JSON data and calls the overloaded onColor function
 * with the deserialized JsonObject.
 *
 * @param json The JSON data to be processed.
 * @param msg The output message.
 * @param relay A flag indicating whether to relay the message.
 * @return True if the processing is successful, false otherwise.
 */
bool JsonProcessor::onColor(const String& json, String& msg, bool relay)
{
	debug_e("JsonProcessor::onColor: %s", json.c_str());
	StaticJsonDocument<400> doc;
	Json::deserialize(doc, json);
	return onColor(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Processes the color command from a JSON object.
 * 
 * This function is responsible for processing the color command from a JSON object.
 * It can handle both single command and multi-command posts.
 * 
 * @param root The JSON object containing the color command.
 * @param msg A reference to a string that will hold any error messages.
 * @param relay A boolean indicating whether to relay the command to the app.
 * @return A boolean indicating the success of the color command processing.
 */
bool JsonProcessor::onColor(JsonObject root, String& msg, bool relay)
{
	bool result = false;
	if(system_get_free_heap_size()<MIN_HEAP_FREE) {
		debug_i("out of memory in processing onColor");
		msg = F("out of memory in processing onColor");
		return false;
	}
	auto cmds = root[F("cmds")].as<JsonArray>();
	if(!cmds.isNull()) {
		Vector<String> errors;
		// multi command post (needs testing)
		debug_i("  multi command post");
		for(unsigned i = 0; i < cmds.size(); ++i) {
			debug_i("command %i: %s", i, cmds[i].as<String>().c_str());
			String msg;
			if(!onSingleColorCommand(cmds[i], msg))
				errors.add(msg);
		}

		if(errors.size() == 0)
			result = true;
		else {
			String msg;
			for(unsigned i = 0; i < errors.size(); ++i)
				msg += String(i) + ": " + errors[i] + "|";
			result = false;
			debug_i("  multi command post, %s", msg.c_str());
		}
	} else {
		debug_i("  single command post %s", msg.c_str());
		if(onSingleColorCommand(root, msg))
			result = true;
		else
			result = false;
	}

	if(relay)
		app.onCommandRelay(F("color"), root);

	return result;
}

/**
 * @brief Handles the "onStop" event for the JsonProcessor class.
 * 
 * This function deserializes the given JSON string into a StaticJsonDocument object
 * and calls the overloaded onStop function with the deserialized JsonObject, a message string,
 * and a boolean flag indicating whether to use the relay.
 * 
 * @param json The JSON string to be deserialized.
 * @param msg The output message string.
 * @param relay Flag indicating whether to use the relay.
 * @return True if the onStop function is successfully called, false otherwise.
 */
bool JsonProcessor::onStop(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onStop(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Handles the "stop" command in the JSON message.
 * 
 * This function stops the animation and performs other necessary actions based on the provided JSON message.
 * 
 * @param root The JSON object containing the command and parameters.
 * @param msg A reference to a string where error messages can be stored.
 * @param relay A boolean indicating whether the command should be relayed to another device.
 * @return true if the command was successfully processed, false otherwise.
 */
bool JsonProcessor::onStop(JsonObject root, String& msg, bool relay)
{
	RequestParameters params;
	JsonProcessor::parseRequestParams(root, params);
	app.rgbwwctrl.clearAnimationQueue(params.channels);
	app.rgbwwctrl.skipAnimation(params.channels);

	onDirect(root, msg, false);

	if(relay) {
		addChannelStatesToCmd(root, params.channels);
		app.onCommandRelay(F("stop"), root);
	}

	return true;
}

/**
 * @brief Skips the animation and performs additional actions based on the provided parameters.
 *
 * This function deserializes the given JSON string into a StaticJsonDocument object and then calls the overloaded
 * onSkip function with the deserialized JsonObject, a message string, and a relay flag.
 *
 * @param json The JSON string to be skipped.
 * @param msg The message string to be passed to the onSkip function.
 * @param relay The relay flag to be passed to the onSkip function.
 * @return True if the onSkip function is successfully called, false otherwise.
 */
bool JsonProcessor::onSkip(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onSkip(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Skips the animation and performs additional actions based on the provided parameters.
 *
 * This function parses the request parameters from the given JSON object and skips the animation
 * for the specified channels. It then calls the onDirect function to perform additional actions.
 * If the relay flag is set to true, it adds the channel states to the command and calls the
 * onCommandRelay function.
 *
 * @param root The JSON object containing the request data.
 * @param msg A reference to a string that will be modified to store any error messages.
 * @param relay A boolean flag indicating whether to relay the command.
 * @return True if the animation was skipped successfully, false otherwise.
 */
bool JsonProcessor::onSkip(JsonObject root, String& msg, bool relay)
{
	RequestParameters params;
	JsonProcessor::parseRequestParams(root, params);
	app.rgbwwctrl.skipAnimation(params.channels);

	onDirect(root, msg, false);

	if(relay) {
		addChannelStatesToCmd(root, params.channels);
		app.onCommandRelay(F("skip"), root);
	}

	return true;
}

/**
 * @brief Pauses the animation and performs additional actions based on the provided parameters.
 *
 * This function deserializes the given JSON string into a StaticJsonDocument,
 * and then calls the onPause function with the deserialized JsonObject.
 *
 * @param json The JSON string to be deserialized.
 * @param msg The output message.
 * @param relay The relay flag.
 * @return True if the onPause function is successfully called, false otherwise.
 */
bool JsonProcessor::onPause(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onPause(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Pauses the animation and performs additional actions based on the provided parameters.
 * 
 * This function pauses the animation by calling `app.rgbwwctrl.pauseAnimation()` with the specified channels.
 * It also calls `onDirect()` with the provided `root` and `msg` parameters.
 * 
 * If the `relay` parameter is true, it adds the channel states to the command and calls `app.onCommandRelay()`
 * with the command "pause" and the `root` parameter.
 * 
 * @param root The JsonObject containing the request data.
 * @param msg A reference to a String object to store any additional message.
 * @param relay A boolean indicating whether to perform additional relay actions.
 * @return true if the function executed successfully, false otherwise.
 */
bool JsonProcessor::onPause(JsonObject root, String& msg, bool relay)
{
	RequestParameters params;
	JsonProcessor::parseRequestParams(root, params);

	app.rgbwwctrl.pauseAnimation(params.channels);

	onDirect(root, msg, false);

	if(relay) {
		addChannelStatesToCmd(root, params.channels);
		app.onCommandRelay(F("pause"), root);
	}

	return true;
}

/**
 * @brief Continues the animation and relays the command if specified.
 * 
 * This function deserializes the JSON data using the StaticJsonDocument class
 * and calls the overloaded onContinue function with the deserialized JsonObject.
 * 
 * @param json The JSON data to be processed.
 * @param msg Output parameter to store any error message.
 * @param relay Flag indicating whether to relay the data or not.
 * @return True if the operation is successful, false otherwise.
 */
bool JsonProcessor::onContinue(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onContinue(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Continues the animation and relays the command if specified.
 *
 * This function is called to continue the animation based on the provided JSON object.
 * It parses the request parameters, continues the animation, and relays the command if the relay flag is set.
 *
 * @param root The JSON object containing the animation parameters.
 * @param msg A reference to a string that will hold any error message.
 * @param relay A boolean flag indicating whether to relay the command or not.
 * @return True if the animation was continued successfully, false otherwise.
 */
bool JsonProcessor::onContinue(JsonObject root, String& msg, bool relay)
{
	RequestParameters params;
	JsonProcessor::parseRequestParams(root, params);
	app.rgbwwctrl.continueAnimation(params.channels);

	if(relay)
		app.onCommandRelay(F("continue"), root);

	return true;
}

/**
 * @brief Handles the "blink" command in the JSON payload.
 * 
 * This function deserializes the JSON data into a StaticJsonDocument,
 * and then calls the overloaded onBlink function with the deserialized
 * JsonObject, message string, and relay flag.
 * 
 * @param json The JSON data to process.
 * @param msg The output message string.
 * @param relay The relay flag.
 * @return True if the blink command was processed successfully, false otherwise.
 */
bool JsonProcessor::onBlink(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onBlink(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Handles system requests.
 * @param root The JSON object containing the command.
 * @param msg Error message if any.
 * @return True on success.
 */
bool JsonProcessor::onSystemReq(JsonObject root, String& msg)
{
	String cmd = root[F("cmd")].as<const char*>();
	if(cmd) {
		if(cmd.equals(F("debug"))) {
			bool enable;
			if(Json::getValue(root[F("enable")], enable)) {
				Serial.systemDebugOutput(enable);
			} else {
				msg = F("Missing enable param");
				return false;
			}
		} else if(!app.delayedCMD(cmd, 1500)) {
			msg = F("Unknown command: ") + cmd;
			return false;
		}
	} else {
		msg = F("Missing cmd param");
		return false;
	}
	return true;
}

/**
 * @brief Handles the "blink" command in the JSON payload.
 * 
 * This function parses the JSON payload and extracts the necessary parameters for the "blink" command.
 * It then calls the `blink` function of the `rgbwwctrl` object with the extracted parameters.
 * If the `relay` flag is set to true, it also calls the `onCommandRelay` function with the "blink" command.
 * 
 * @param root The JSON object containing the command and its parameters.
 * @param msg A reference to a string that will hold any error message generated during the processing.
 * @param relay A boolean flag indicating whether to relay the command or not.
 * @return Returns true if the command was successfully processed, false otherwise.
 */
bool JsonProcessor::onBlink(JsonObject root, String& msg, bool relay)
{
	RequestParameters params;
	params.ramp.value = 500; //default

	JsonProcessor::parseRequestParams(root, params);

	app.rgbwwctrl.blink(params.channels, params.ramp.value, params.queue, params.requeue, params.name);

	if(relay)
		app.onCommandRelay(F("blink"), root);

	return true;
}

/**
 * @brief Toggles the RGBWW control and sends a command relay if specified.
 *
 * This function deserializes the JSON data and calls the overloaded onToggle function
 * with the deserialized JsonObject, message string, and relay flag.
 *
 * @param json The JSON data to be processed.
 * @param msg The output message string.
 * @param relay The relay flag.
 * @return True if the toggle operation is successful, false otherwise.
 */
bool JsonProcessor::onToggle(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onToggle(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Toggles the RGBWW control and sends a command relay if specified.
 * 
 * @param root The JSON object containing the command.
 * @param msg The message to be modified.
 * @param relay Flag indicating whether to send a command relay.
 * @return true if the toggle was successful, false otherwise.
 */
bool JsonProcessor::onToggle(JsonObject root, String& msg, bool relay)
{
	app.rgbwwctrl.toggle();

	if(relay)
		app.onCommandRelay(F("toggle"), root);

	return true;
}

/**
 * @brief Handles a single color command from a JSON object.
 * 
 * This function parses the request parameters from the JSON object and performs the corresponding action
 * based on the parameters. It supports both HSV and RAW color modes. If the parameters are valid and the
 * action is successfully executed, it returns true. Otherwise, it returns false and sets the errorMsg
 * parameter with an appropriate error message.
 * 
 * @param root The JSON object containing the command parameters.
 * @param errorMsg A reference to a string variable to store the error message, if any.
 * @return Returns true if the command is executed successfully, false otherwise.
 */
bool JsonProcessor::onSingleColorCommand(JsonObject root, String& errorMsg)
{
	RequestParameters params;
	parseRequestParams(root, params);
	if(params.checkParams(errorMsg) != 0) {
		debug_i("checkParams failed:",errorMsg.c_str());
		return false;
	}

	bool queueOk = false;
	if(params.mode == RequestParameters::Mode::Hsv) {
		if(!params.hasHsvFrom) {
			if(params.cmd == F("fade")) {
				queueOk = app.rgbwwctrl.fadeHSV(params.hsv, params.ramp, params.direction, params.queue, params.requeue,
												params.name);
			} else {
				queueOk =
					app.rgbwwctrl.setHSV(params.hsv, params.ramp.value, params.queue, params.requeue, params.name);
			}
		} else {
			app.rgbwwctrl.fadeHSV(params.hsvFrom, params.hsv, params.ramp, params.direction, params.queue);
		}
	} else if(params.mode == RequestParameters::Mode::Raw) {
		if(!params.hasRawFrom) {
			if(params.cmd == F("fade")) {
				queueOk = app.rgbwwctrl.fadeRAW(params.raw, params.ramp, params.queue);
			} else {
				queueOk = app.rgbwwctrl.setRAW(params.raw, params.ramp.value, params.queue);
			}
		} else {
			app.rgbwwctrl.fadeRAW(params.rawFrom, params.raw, params.ramp, params.queue);
		}
	} else {
		errorMsg = F("No color object!");
		debug_i("no color object");
		return false;
	}

	if(!queueOk) {
		debug_i("queue full");
		errorMsg = F("Queue full");
	}
	return queueOk;
}

/**
 * @brief Handles a direct JSON command.
 *
 * This function processes a direct JSON command and performs the corresponding action based on the provided parameters.
 * 
 * This function deserializes the given JSON string into a JSON document and calls the overloaded
 * `onDirect` function with the deserialized JSON object.
 *
 * @param json The JSON string to be processed.
 * @param msg Output parameter to store any error message.
 * @param relay Flag indicating whether to relay the message.
 * @return True if the processing is successful, false otherwise.
 */
bool JsonProcessor::onDirect(const String& json, String& msg, bool relay)
{
	StaticJsonDocument<256> doc;
	Json::deserialize(doc, json);
	return onDirect(doc.as<JsonObject>(), msg, relay);
}

/**
 * @brief Handles a direct JSON command.
 *
 * This function processes a direct JSON command and performs the corresponding action based on the provided parameters.
 *
 * @param root The JSON object containing the command parameters.
 * @param msg A reference to a string that will be updated with a message indicating the result of the command.
 * @param relay A boolean value indicating whether the command should be relayed to another component.
 * @return Returns true if the command was successfully processed, false otherwise.
 */
bool JsonProcessor::onDirect(JsonObject root, String& msg, bool relay)
{
	RequestParameters params;
	JsonProcessor::parseRequestParams(root, params);

	if(params.mode == RequestParameters::Mode::Hsv) {
		app.rgbwwctrl.colorDirectHSV(params.hsv);
	} else if(params.mode == RequestParameters::Mode::Raw) {
		app.rgbwwctrl.colorDirectRAW(params.raw);
	} else {
		msg = F("No color object!");
	}

	if(relay)
		app.onCommandRelay(F("direct"), root);

	return true;
}

/**
 * @brief Parses the request parameters from a JSON object.
 *
 * This function extracts the request parameters from the provided JSON object and populates
 * the RequestParameters object accordingly.
 *
 * @param root The JSON object containing the request parameters.
 * @param params The RequestParameters object to be populated.
 */
void JsonProcessor::parseRequestParams(JsonObject root, RequestParameters& params)
{
	String value;

	JsonObject hsv = root[F("hsv")];
	if(!hsv.isNull()) {
		params.mode = RequestParameters::Mode::Hsv;
		if(Json::getValue(hsv[F("h")], value))
			params.hsv.h = AbsOrRelValue(value, AbsOrRelValue::Type::Hue);
		if(Json::getValue(hsv[F("s")], value))
			params.hsv.s = AbsOrRelValue(value);
		if(Json::getValue(hsv[F("v")], value))
			params.hsv.v = AbsOrRelValue(value);
		if(Json::getValue(hsv[F("ct")], value))
			params.hsv.ct = AbsOrRelValue(value, AbsOrRelValue::Type::Ct);

		JsonObject from = hsv[F("from")];
		if(!from.isNull()) {
			params.hasHsvFrom = true;
			if(Json::getValue(from[F("h")], value))
				params.hsv.h = AbsOrRelValue(value, AbsOrRelValue::Type::Hue);
			if(Json::getValue(from[F("s")], value))
				params.hsv.s = AbsOrRelValue(value);
			if(Json::getValue(from[F("v")], value))
				params.hsv.v = AbsOrRelValue(value);
			if(Json::getValue(from[F("ct")], value))
				params.hsv.ct = AbsOrRelValue(value, AbsOrRelValue::Type::Ct);
		}
	} else if(!root[F("raw")].isNull()) {
		JsonObject raw = root[F("raw")];
		params.mode = RequestParameters::Mode::Raw;
		if(Json::getValue(raw[F("r")], value))
			params.raw.r = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
		if(Json::getValue(raw[F("g")], value))
			params.raw.g = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
		if(Json::getValue(raw[F("b")], value))
			params.raw.b = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
		if(Json::getValue(raw[F("ww")], value))
			params.raw.ww = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
		if(Json::getValue(raw[F("cw")], value))
			params.raw.cw = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);

		JsonObject from = raw[F("from")];
		if(!from.isNull()) {
			params.hasRawFrom = true;
			if(Json::getValue(from[F("r")], value))
				params.rawFrom.r = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
			if(Json::getValue(from[F("g")], value))
				params.rawFrom.g = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
			if(Json::getValue(from[F("b")], value))
				params.rawFrom.b = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
			if(Json::getValue(from[F("ww")], value))
				params.rawFrom.ww = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
			if(Json::getValue(from[F("cw")], value))
				params.rawFrom.cw = AbsOrRelValue(value, AbsOrRelValue::Type::Raw);
		}
	}

	if(Json::getValue(root[F("t")], params.ramp.value)) {
		params.ramp.type = RampTimeOrSpeed::Type::Time;
	}

	if(Json::getValue(root[F("s")], params.ramp.value)) {
		params.ramp.type = RampTimeOrSpeed::Type::Speed;
	}

	if(!root[F("r")].isNull()) {
		params.requeue = root[F("r")].as<bool>();
	}

	Json::getValue(root[F("d")], params.direction);

	Json::getValue(root[F("name")], params.name);

	Json::getValue(root[F("cmd")], params.cmd);

	if(!root[F("q")].isNull()) {
		String q = root[F("q")];
		if(q == F("back"))
			params.queue = QueuePolicy::Back;
		else if(q == F("front"))
			params.queue = QueuePolicy::Front;
		else if(q == F("front_reset"))
			params.queue = QueuePolicy::FrontReset;
		else if(q == F("single"))
			params.queue = QueuePolicy::Single;
		else {
			params.queue = QueuePolicy::Invalid;
		}
	}

	JsonArray arr;
	if(Json::getValue(root[F("channels")], arr)) {
		for(size_t i = 0; i < arr.size(); ++i) {
			String str = arr[i];
			if(str == F("h")) {
				params.channels.add(CtrlChannel::Hue);
			} else if(str == F("s")) {
				params.channels.add(CtrlChannel::Sat);
			} else if(str == F("v")) {
				params.channels.add(CtrlChannel::Val);
			} else if(str == F("ct")) {
				params.channels.add(CtrlChannel::ColorTemp);
			} else if(str == F("r")) {
				params.channels.add(CtrlChannel::Red);
			} else if(str == F("g")) {
				params.channels.add(CtrlChannel::Green);
			} else if(str == F("b")) {
				params.channels.add(CtrlChannel::Blue);
			} else if(str == F("ww")) {
				params.channels.add(CtrlChannel::WarmWhite);
			} else if(str == F("cw")) {
				params.channels.add(CtrlChannel::ColdWhite);
			}
		}
	}
}

/**
 * @brief Check the parameters of the RequestParameters object.
 *
 * This function checks the parameters of the RequestParameters object and returns an error message if any parameter is invalid.
 *
 * @param errorMsg The error message to be returned if any parameter is invalid.
 * @return An integer indicating the result of the parameter check. 0 if all parameters are valid, non-zero otherwise.
 */
int JsonProcessor::RequestParameters::checkParams(String& errorMsg) const
{
	if(mode == Mode::Hsv) {
		if(hsv.ct.hasValue()) {
			if(hsv.ct != 0 && (hsv.ct < 100 || hsv.ct > 10000 || (hsv.ct > 500 && hsv.ct < 2000))) {
				errorMsg = F("bad param for ct");
				return 1;
			}
		}

		if(!hsv.h.hasValue() && !hsv.s.hasValue() && !hsv.v.hasValue() && !hsv.ct.hasValue()) {
			errorMsg = F("Need at least one HSVCT component!");
			return 1;
		}
	} else if(mode == Mode::Raw) {
		if(!raw.r.hasValue() && !raw.g.hasValue() && !raw.b.hasValue() && !raw.ww.hasValue() && !raw.cw.hasValue()) {
			errorMsg = F("Need at least one RAW component!");
			return 1;
		}
	}

	if(queue == QueuePolicy::Invalid) {
		errorMsg = F("Invalid queue policy");
		return 1;
	}

	if(cmd != F("fade") && cmd != F("solid")) {
		errorMsg = F("Invalid cmd");
		return 1;
	}

	if(direction < 0 || direction > 1) {
		errorMsg = F("Invalid direction");
		return 1;
	}

	if(ramp.type == RampTimeOrSpeed::Type::Speed && ramp.value == 0) {
		errorMsg = F("Speed cannot be 0!");
		return 1;
	}

	return 0;
}

/**
 * @brief Handles the JSON-RPC request.
 *
 * This function processes the JSON-RPC request and performs the necessary actions based on the received JSON data.
 *
 * @param json The JSON string containing the request.
 * @param response The JSON string containing the response.
 * @return True if the request was successfully processed, false otherwise.
 */
bool JsonProcessor::onJsonRpc(const String& json, String& response)
{
	debug_d("JsonProcessor::onJsonRpc: %s\n", json.c_str());
	JsonRpcMessageIn rpc(json);

	String msg;
	String method = rpc.getMethod();
	bool success = false;
	int errorCode = 0;

    ApiResponse resp;
    DynamicJsonDocument doc(4096);
    resp.data = doc.to<JsonObject>();

    JsonProcessor::RequestType type = JsonProcessor::RequestType::Command;

    // Auto-detect Query type for specific methods if params are empty
    if (rpc.getParams().isNull() || rpc.getParams().size() == 0) {
        if (method == F("info") || method == F("status") || method == F("networks") || method == F("color") || method == F("system") || method == F("config")) {
            type = JsonProcessor::RequestType::Query;
        }
    }

    handleRequest(method, type, rpc.getParams(), resp, false);

    if (resp.code == 200) {
        success = true;
    } else if (resp.code == 404 || resp.code == 405) {
        msg = F("Method not found");
        errorCode = -32601;
        success = false;
    } else {
        msg = resp.message;
        errorCode = -32603; // Internal error
        success = false;
    }

	if (!success && errorCode == 0) {
		errorCode = -32603; // Internal error
		if (msg.length() == 0) msg = F("Internal Error");
	}

	// Check if notification (no id)
	JsonObject root = rpc.getRoot();
	if (root.containsKey("id")) {
		DynamicJsonDocument responseDoc(512);
		responseDoc[F("jsonrpc")] = F("2.0");
		responseDoc[F("id")] = root[F("id")];
		
		if (success) {
            // If response data is available (for query), use it. otherwise OK.
            if (resp.data.size() > 0) {
			    responseDoc[F("result")] = resp.data;
            } else {
                responseDoc[F("result")] = F("OK");
            }
		} else {
			JsonObject error = responseDoc.createNestedObject(F("error"));
			error[F("code")] = errorCode;
			error[F("message")] = msg;
		}
		serializeJson(responseDoc, response);
	}

	return success;
}

bool JsonProcessor::onJsonRpc(const String& json)
{
	String response;
	return onJsonRpc(json, response);
}

/**
 * @brief Adds channel states to the command JSON object.
 * 
 * This function adds channel states to the command JSON object based on the current color mode.
 * If the color mode is HSV, the function adds the hue, saturation, value, and color temperature channels.
 * If the color mode is Raw, the function adds the red, green, blue, warm white, and cold white channels.
 * 
 * @param root The root JSON object to which the channel states will be added.
 * @param channels The list of channels for which the states will be added.
 */
void JsonProcessor::addChannelStatesToCmd(JsonObject root, const RGBWWLed::ChannelList& channels)
{
	switch(app.rgbwwctrl.getMode()) {
	case RGBWWLed::ColorMode::Hsv: {
		const HSVCT& c = app.rgbwwctrl.getCurrentColor();
		JsonObject obj = root.createNestedObject(F("hsv"));
		if(channels.count() == 0 || channels.contains(CtrlChannel::Hue))
			obj[F("h")] = (float(c.h) / float(RGBWW_CALC_HUEWHEELMAX)) * 360.0;
		if(channels.count() == 0 || channels.contains(CtrlChannel::Sat))
			obj[F("s")] = (float(c.s) / float(RGBWW_CALC_MAXVAL)) * 100.0;
		if(channels.count() == 0 || channels.contains(CtrlChannel::Val))
			obj[F("v")] = (float(c.v) / float(RGBWW_CALC_MAXVAL)) * 100.0;
		if(channels.count() == 0 || channels.contains(CtrlChannel::ColorTemp))
			obj[F("ct")] = c.ct;
		break;
	}
	case RGBWWLed::ColorMode::Raw: {
		const ChannelOutput& c = app.rgbwwctrl.getCurrentOutput();
		JsonObject obj = root.createNestedObject(F("raw"));
		if(channels.count() == 0 || channels.contains(CtrlChannel::Red))
			obj[F("r")] = c.r;
		if(channels.count() == 0 || channels.contains(CtrlChannel::Green))
			obj[F("g")] = c.g;
		if(channels.count() == 0 || channels.contains(CtrlChannel::Blue))
			obj[F("b")] = c.b;
		if(channels.count() == 0 || channels.contains(CtrlChannel::WarmWhite))
			obj[F("ww")] = c.ww;
		if(channels.count() == 0 || channels.contains(CtrlChannel::ColdWhite))
			obj[F("cw")] = c.cw;
		break;
	}
	}
}
bool JsonProcessor::onSetOn(JsonObject root, String& msg, bool relay) {
	RequestParameters params;
	parseRequestParams(root, params);

	app.rgbwwctrl.setOn(
		params.channels,
		params.direction,
		params.ramp,
		params.queue,
		params.requeue,
		params.name
	);
	// Optionally relay or set msg
	return true;
}

bool JsonProcessor::onSetOff(JsonObject root, String& msg, bool relay) {
	RequestParameters params;
	parseRequestParams(root, params);

	// If no color specified and mode is HSV, use current HSV but set v=0
	bool hasColor = params.mode == RequestParameters::Mode::Hsv &&
		(params.hsv.h.hasValue() || params.hsv.s.hasValue() || params.hsv.v.hasValue() || params.hsv.ct.hasValue());

	if (!hasColor && app.rgbwwctrl.getMode() == RGBWWLed::ColorMode::Hsv) {
		HSVCT current = app.rgbwwctrl.getCurrentColor();
		params.hsv = current;
		params.mode = RequestParameters::Mode::Hsv;
		params.hsv.v = 0;
	}

	app.rgbwwctrl.setOff(
		params.channels,
		params.direction,
		params.ramp,
		params.queue,
		params.requeue,
		params.name
	);
	// Optionally relay or set msg
	return true;
}

void JsonProcessor::serializeState(JsonObject root, bool includeRaw, bool includeHsv)
{
	if (includeRaw) {
		JsonObject raw = root.createNestedObject(F("raw"));
		ChannelOutput output = app.rgbwwctrl.getCurrentOutput();
		raw[F("r")] = output.r;
		raw[F("g")] = output.g;
		raw[F("b")] = output.b;
		raw[F("ww")] = output.ww;
		raw[F("cw")] = output.cw;
	}

	if (includeHsv) {
		JsonObject hsv = root.createNestedObject(F("hsv"));
		float h, s, v;
		int ct;
		HSVCT c = app.rgbwwctrl.getCurrentColor();
		c.asRadian(h, s, v, ct);
		hsv[F("h")] = h;
		hsv[F("s")] = s;
		hsv[F("v")] = v;
		hsv[F("ct")] = ct;
	}
}

void JsonProcessor::serializeInfo(JsonObject data)
{
	data[F("deviceid")] = system_get_chip_id();
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
	data[F("current_rom")] = String(app.ota.getRomPartition().name());
#endif
	data[F("git_version")] = fw_git_version;
	data[F("build_type")] = BUILD_TYPE;
	data[F("git_date")] = fw_git_date;
	data[F("webapp_version")] = WEBAPP_VERSION;
	data[F("sming")] = SMING_VERSION;
	data[F("event_num_clients")] = app.eventserver.activeClients;
	data[F("uptime")] = app.getUptime();
	data[F("cpu_usage_percent")] = app.getCpuPercent();
	data[F("heap_free")] = system_get_free_heap_size();
	data[F("soc")]=SOC;
	
	JsonObject rgbww = data.createNestedObject(F("rgbww"));
	rgbww[F("version")] = RGBWW_VERSION;
	rgbww[F("queuesize")] = RGBWW_ANIMATIONQSIZE;

	JsonObject con = data.createNestedObject(F("connection"));
	con[F("connected")] = WifiStation.isConnected();
	con[F("ssid")] = WifiStation.getSSID();
	con[F("dhcp")] = WifiStation.isEnabledDHCP();
	con[F("ip")] = WifiStation.getIP().toString();
	con[F("netmask")] = WifiStation.getNetworkMask().toString();
	con[F("gateway")] = WifiStation.getNetworkGateway().toString();
	con[F("mac")] = WifiStation.getMAC();
}


void JsonProcessor::serializeNetworks(JsonObject root)
{
	if(app.network.isScanning()) {
		root[F("scanning")] = true;
	} else {
		root[F("scanning")] = false;
		JsonArray netlist = root.createNestedArray(F("available"));
		BssList networks = app.network.getAvailableNetworks();
		for(unsigned int i = 0; i < networks.count(); i++) {
			if(networks[i].hidden)
				continue;

			// SSIDs may contain any byte values. Some are not printable and will cause the javascript client to fail
			// on parsing the message. Try to filter those here
            bool printable = true;
            for(unsigned int j = 0; j < networks[i].ssid.length(); ++j) {
                if(networks[i].ssid[j] < 0x20) {
                    printable = false;
                    break;
                }
            }

			if(!printable) {
				debug_w("Filtered SSID due to unprintable characters: %s", networks[i].ssid.c_str());
				continue;
			}

			JsonObject item = netlist.createNestedObject();
			item[F("id")] = (int)networks[i].getHashId();
			item[F("ssid")] = networks[i].ssid;
			item[F("signal")] = networks[i].rssi;
			item[F("encryption")] = networks[i].getAuthorizationMethodName();
			//limit to max 25 networks
			if(i >= 25)
				break;
		}
	}
}


void JsonProcessor::handleRequest(String endpoint, RequestType type, JsonObject payload, ApiResponse& resp, bool relay) {
    if (endpoint == F("color")) {
        if (type == RequestType::Query) {
            serializeState(resp.data);
        } else if (type == RequestType::Command) {
            String msg;
            if (!onColor(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        }
    } else if (endpoint == F("info")) {
        if (type == RequestType::Query) {
            serializeInfo(resp.data);
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("stop")) {
        if (type == RequestType::Command) {
            String msg;
            if (!onStop(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("pause")) {
        if (type == RequestType::Command) {
             String msg;
             if (!onPause(payload, msg, relay)) {
                 resp.code = 400;
                 resp.message = msg;
             }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("continue")) {
        if (type == RequestType::Command) {
            String msg;
            if (!onContinue(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("skip")) {
         if (type == RequestType::Command) {
            String msg;
            if (!onSkip(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
     } else if (endpoint == F("blink")) {
         if (type == RequestType::Command) {
            String msg;
            if (!onBlink(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("toggle")) {
         if (type == RequestType::Command) {
            String msg;
            if (!onToggle(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("on")) {
         if (type == RequestType::Command) {
            String msg;
            if (!onSetOn(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("off")) {
         if (type == RequestType::Command) {
            String msg;
            if (!onSetOff(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("direct")) {
         if (type == RequestType::Command) {
            String msg;
            if (!onDirect(payload, msg, relay)) {
                resp.code = 400;
                resp.message = msg;
            }
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("networks")) {
        if (type == RequestType::Query) {
            serializeNetworks(resp.data);
            resp.code = 200;
        } else if (type == RequestType::Command) {
            if(!app.network.isScanning()) {
                app.network.scan(false);
            }
            resp.code = 200;
        } else {
            resp.code = 405;
            resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("system")) {
        if (type == RequestType::Command) {
             String msg;
             if (!onSystemReq(payload, msg)) {
                 resp.code = 400;
                 resp.message = msg;
             }
        } else {
             resp.code = 405;
             resp.message = F("Method Not Allowed");
        }
    } else if (endpoint == F("config")) {
        // Config is too large to buffer in memory - must be handled via streaming at the transport level
        resp.code = 501;
        resp.message = F("Config requires streaming transport");
    } else {
        resp.code = 404;
        resp.message = F("Endpoint not found");
    }
}
