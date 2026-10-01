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


#include <RGBWWCtrl.h>
#include <apihandler.h>

#define MIN_HEAP_FREE 4096

namespace {
bool parseAbsOrRelValue(const JsonVariantConst& source, Optional<AbsOrRelValue>& target,
						AbsOrRelValue::Type type = AbsOrRelValue::Type::Percent)
{
	if(source.isNull()) {
		return false;
	}

	if(source.is<const char*>()) {
		const char* value = source.as<const char*>();
		if(value != nullptr && value[0] != '\0') {
			target = AbsOrRelValue(value, type);
			return true;
		}
		return false;
	}

	if(source.is<float>() || source.is<double>()) {
		target = AbsOrRelValue(static_cast<float>(source.as<double>()), type);
		return true;
	}

	if(source.is<int>() || source.is<long>() || source.is<unsigned int>() || source.is<unsigned long>()) {
		target = AbsOrRelValue(static_cast<int>(source.as<long>()), type);
		return true;
	}

	String value;
	if(Json::getValue(source, value)) {
		target = AbsOrRelValue(value, type);
		return true;
	}

	return false;
}
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
	RequestParameters params;
	std::vector<RequestParameters> batch;
	auto cmds = root[F("cmds")].as<JsonArray>();
	if(cmds.isNull()) {
		parseRequestParams(root, params);
	} else {
		for(JsonObject item : cmds) {
			batch.emplace_back();
			parseRequestParams(item, batch.back());
		}
	}

	const bool result = runColor(params, batch, msg);

	if(relay)
		app.onCommandRelay(F("color"), root);

	return result;
}

/**
 * @brief Executes a single color command, or each entry of a "cmds" batch if non-empty.
 */
bool JsonProcessor::runColor(RequestParameters& params, std::vector<RequestParameters>& batch, String& errorMsg)
{
	if(!app.checkHeap(MIN_HEAP_FREE)) {
		debug_i(ANSI_COLOR_BLUE "out of memory in processing onColor" ANSI_COLOR_RESET);
		errorMsg = F("out of memory in processing onColor");
		return false;
	}

	if(batch.empty()) {
		return runColorCommand(params, errorMsg);
	}

	debug_i(ANSI_COLOR_BLUE "  multi command post" ANSI_COLOR_RESET);
	String errors;
	for(unsigned i = 0; i < batch.size(); ++i) {
		String itemError;
		if(!runColorCommand(batch[i], itemError)) {
			if(errors.length() > 0)
				errors += '|';
			errors.concat(i);
			errors += ": ";
			errors += itemError;
		}
	}

	if(errors.length() == 0) {
		return true;
	}
	debug_i(ANSI_COLOR_BLUE "  multi command post, " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, errors.c_str());
	errorMsg = errors;
	return false;
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
	parseRequestParams(root, params);
	runStop(params, msg);

	if(relay) {
		addChannelStatesToCmd(root, params.channels);
		app.onCommandRelay(F("stop"), root);
	}

	return true;
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
	parseRequestParams(root, params);
	runSkip(params, msg);

	if(relay) {
		addChannelStatesToCmd(root, params.channels);
		app.onCommandRelay(F("skip"), root);
	}

	return true;
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
	parseRequestParams(root, params);
	runPause(params, msg);

	if(relay) {
		addChannelStatesToCmd(root, params.channels);
		app.onCommandRelay(F("pause"), root);
	}

	return true;
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
	parseRequestParams(root, params);
	runContinue(params);

	if(relay)
		app.onCommandRelay(F("continue"), root);

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

	parseRequestParams(root, params);
	runBlink(params);

	if(relay)
		app.onCommandRelay(F("blink"), root);

	return true;
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
	runToggle();

	if(relay)
		app.onCommandRelay(F("toggle"), root);

	return true;
}

void JsonProcessor::runStop(const RequestParameters& params, String& msg)
{
	app.rgbwwctrl.clearAnimationQueue(params.channels);
	app.rgbwwctrl.skipAnimation(params.channels);
	runDirect(params, msg);
}

void JsonProcessor::runSkip(const RequestParameters& params, String& msg)
{
	app.rgbwwctrl.skipAnimation(params.channels);
	runDirect(params, msg);
}

void JsonProcessor::runPause(const RequestParameters& params, String& msg)
{
	app.rgbwwctrl.pauseAnimation(params.channels);
	runDirect(params, msg);
}

void JsonProcessor::runContinue(const RequestParameters& params)
{
	app.rgbwwctrl.continueAnimation(params.channels);
}

void JsonProcessor::runBlink(const RequestParameters& params)
{
	app.rgbwwctrl.blink(params.channels, params.ramp.value, params.queue, params.requeue, params.name);
}

void JsonProcessor::runToggle()
{
	app.rgbwwctrl.toggle();
}

/**
 * @brief Shared post-parse execution core for a single color command.
 */
bool JsonProcessor::runColorCommand(RequestParameters& params, String& errorMsg)
{
	if(params.checkParams(errorMsg) != 0) {
		debug_i(ANSI_COLOR_BLUE "checkParams failed:" ANSI_COLOR_RESET,errorMsg.c_str());
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
		debug_i(ANSI_COLOR_BLUE "no color object" ANSI_COLOR_RESET);
		return false;
	}

	if(!queueOk) {
		debug_i(ANSI_COLOR_BLUE "queue full" ANSI_COLOR_RESET);
		errorMsg = F("Queue full");
	}
	return queueOk;
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
	parseRequestParams(root, params);
	runDirect(params, msg);

	if(relay)
		app.onCommandRelay(F("direct"), root);

	return true;
}

void JsonProcessor::runDirect(const RequestParameters& params, String& msg)
{
	if(params.mode == RequestParameters::Mode::Hsv) {
		app.rgbwwctrl.colorDirectHSV(params.hsv);
	} else if(params.mode == RequestParameters::Mode::Raw) {
		app.rgbwwctrl.colorDirectRAW(params.raw);
	} else {
		msg = F("No color object!");
	}
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
	JsonObject hsv = root[F("hsv")];
	if(!hsv.isNull()) {
		params.mode = RequestParameters::Mode::Hsv;
		parseAbsOrRelValue(hsv[F("h")], params.hsv.h, AbsOrRelValue::Type::Hue);
		parseAbsOrRelValue(hsv[F("s")], params.hsv.s);
		parseAbsOrRelValue(hsv[F("v")], params.hsv.v);
		parseAbsOrRelValue(hsv[F("ct")], params.hsv.ct, AbsOrRelValue::Type::Ct);

		JsonObject from = hsv[F("from")];
		if(!from.isNull()) {
			params.hasHsvFrom = true;
			parseAbsOrRelValue(from[F("h")], params.hsvFrom.h, AbsOrRelValue::Type::Hue);
			parseAbsOrRelValue(from[F("s")], params.hsvFrom.s);
			parseAbsOrRelValue(from[F("v")], params.hsvFrom.v);
			parseAbsOrRelValue(from[F("ct")], params.hsvFrom.ct, AbsOrRelValue::Type::Ct);
		}
	} else if(!root[F("raw")].isNull()) {
		JsonObject raw = root[F("raw")];
		params.mode = RequestParameters::Mode::Raw;
		parseAbsOrRelValue(raw[F("r")], params.raw.r, AbsOrRelValue::Type::Raw);
		parseAbsOrRelValue(raw[F("g")], params.raw.g, AbsOrRelValue::Type::Raw);
		parseAbsOrRelValue(raw[F("b")], params.raw.b, AbsOrRelValue::Type::Raw);
		parseAbsOrRelValue(raw[F("ww")], params.raw.ww, AbsOrRelValue::Type::Raw);
		parseAbsOrRelValue(raw[F("cw")], params.raw.cw, AbsOrRelValue::Type::Raw);

		JsonObject from = raw[F("from")];
		if(!from.isNull()) {
			params.hasRawFrom = true;
			parseAbsOrRelValue(from[F("r")], params.rawFrom.r, AbsOrRelValue::Type::Raw);
			parseAbsOrRelValue(from[F("g")], params.rawFrom.g, AbsOrRelValue::Type::Raw);
			parseAbsOrRelValue(from[F("b")], params.rawFrom.b, AbsOrRelValue::Type::Raw);
			parseAbsOrRelValue(from[F("ww")], params.rawFrom.ww, AbsOrRelValue::Type::Raw);
			parseAbsOrRelValue(from[F("cw")], params.rawFrom.cw, AbsOrRelValue::Type::Raw);
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
		const char* q = root[F("q")] | "";
		if(strcmp(q, "back") == 0)
			params.queue = QueuePolicy::Back;
		else if(strcmp(q, "front") == 0)
			params.queue = QueuePolicy::Front;
		else if(strcmp(q, "front_reset") == 0)
			params.queue = QueuePolicy::FrontReset;
		else if(strcmp(q, "single") == 0)
			params.queue = QueuePolicy::Single;
		else {
			params.queue = QueuePolicy::Invalid;
		}
	}

	JsonArray arr;
	if(Json::getValue(root[F("channels")], arr)) {
		for(size_t i = 0; i < arr.size(); ++i) {
			const char* str = arr[i] | "";
			if(strcmp(str, "h") == 0) {
				params.channels.add(CtrlChannel::Hue);
			} else if(strcmp(str, "s") == 0) {
				params.channels.add(CtrlChannel::Sat);
			} else if(strcmp(str, "v") == 0) {
				params.channels.add(CtrlChannel::Val);
			} else if(strcmp(str, "ct") == 0) {
				params.channels.add(CtrlChannel::ColorTemp);
			} else if(strcmp(str, "r") == 0) {
				params.channels.add(CtrlChannel::Red);
			} else if(strcmp(str, "g") == 0) {
				params.channels.add(CtrlChannel::Green);
			} else if(strcmp(str, "b") == 0) {
				params.channels.add(CtrlChannel::Blue);
			} else if(strcmp(str, "ww") == 0) {
				params.channels.add(CtrlChannel::WarmWhite);
			} else if(strcmp(str, "cw") == 0) {
				params.channels.add(CtrlChannel::ColdWhite);
			}
		}
	}
}

/**
 * @brief Converts imported command-request fields (or a "cmds" item) to RequestParameters.
 *
 * The raw/hsv component fields are string-value typed in the schema (see
 * command-request-item in params.cfgdb) so they can carry AbsOrRelValue's
 * absolute/relative/percentage token forms; an empty String means the field was absent.
 */
template <typename Fields> void parseCommandRequestFields(Fields root, JsonProcessor::RequestParameters& params)
{
	using RequestParameters = JsonProcessor::RequestParameters;
	auto parseField = [](const String& value, Optional<AbsOrRelValue>& target,
						  AbsOrRelValue::Type type = AbsOrRelValue::Type::Percent) {
		if(value.length() > 0) {
			target = AbsOrRelValue(value.c_str(), type);
		}
	};

	String hsvH = root.hsv.getH();
	String hsvS = root.hsv.getS();
	String hsvV = root.hsv.getV();
	String hsvCt = root.hsv.getCt();
	if(hsvH.length() > 0 || hsvS.length() > 0 || hsvV.length() > 0 || hsvCt.length() > 0) {
		params.mode = RequestParameters::Mode::Hsv;
		parseField(hsvH, params.hsv.h, AbsOrRelValue::Type::Hue);
		parseField(hsvS, params.hsv.s);
		parseField(hsvV, params.hsv.v);
		parseField(hsvCt, params.hsv.ct, AbsOrRelValue::Type::Ct);

		String fromH = root.hsv.from.getH();
		String fromS = root.hsv.from.getS();
		String fromV = root.hsv.from.getV();
		String fromCt = root.hsv.from.getCt();
		if(fromH.length() > 0 || fromS.length() > 0 || fromV.length() > 0 || fromCt.length() > 0) {
			params.hasHsvFrom = true;
			parseField(fromH, params.hsvFrom.h, AbsOrRelValue::Type::Hue);
			parseField(fromS, params.hsvFrom.s);
			parseField(fromV, params.hsvFrom.v);
			parseField(fromCt, params.hsvFrom.ct, AbsOrRelValue::Type::Ct);
		}
	} else {
		String rawR = root.raw.getR();
		String rawG = root.raw.getG();
		String rawB = root.raw.getB();
		String rawWw = root.raw.getWw();
		String rawCw = root.raw.getCw();
		if(rawR.length() > 0 || rawG.length() > 0 || rawB.length() > 0 || rawWw.length() > 0 || rawCw.length() > 0) {
			params.mode = RequestParameters::Mode::Raw;
			parseField(rawR, params.raw.r, AbsOrRelValue::Type::Raw);
			parseField(rawG, params.raw.g, AbsOrRelValue::Type::Raw);
			parseField(rawB, params.raw.b, AbsOrRelValue::Type::Raw);
			parseField(rawWw, params.raw.ww, AbsOrRelValue::Type::Raw);
			parseField(rawCw, params.raw.cw, AbsOrRelValue::Type::Raw);

			String fromR = root.raw.from.getR();
			String fromG = root.raw.from.getG();
			String fromB = root.raw.from.getB();
			String fromWw = root.raw.from.getWw();
			String fromCw = root.raw.from.getCw();
			if(fromR.length() > 0 || fromG.length() > 0 || fromB.length() > 0 || fromWw.length() > 0 ||
			   fromCw.length() > 0) {
				params.hasRawFrom = true;
				parseField(fromR, params.rawFrom.r, AbsOrRelValue::Type::Raw);
				parseField(fromG, params.rawFrom.g, AbsOrRelValue::Type::Raw);
				parseField(fromB, params.rawFrom.b, AbsOrRelValue::Type::Raw);
				parseField(fromWw, params.rawFrom.ww, AbsOrRelValue::Type::Raw);
				parseField(fromCw, params.rawFrom.cw, AbsOrRelValue::Type::Raw);
			}
		}
	}

	// NOTE: unlike the JsonObject version, "t"/"s" presence can't be distinguished from an
	// explicit 0 once imported (ConfigDB's non-negative-integer getters always return a
	// definite uint32_t, defaulting to 0 when absent). An explicit "s":0 is therefore treated
	// the same as "s" being absent (stays Time-mode) instead of flipping to Speed-mode and then
	// failing checkParams()'s "Speed cannot be 0!" check - a narrow, documented behavior
	// difference for a nonsensical input (zero speed) that was rejected either way.
	uint32_t t = root.getT();
	if(t != 0) {
		params.ramp.value = t;
		params.ramp.type = RampTimeOrSpeed::Type::Time;
	}
	uint32_t s = root.getS();
	if(s != 0) {
		params.ramp.value = s;
		params.ramp.type = RampTimeOrSpeed::Type::Speed;
	}

	params.requeue = root.getR();

	// "d" schema default is 1 (see params.cfgdb), matching RequestParameters' own default,
	// so this can read unconditionally without an absent-vs-explicit-0 ambiguity.
	params.direction = int32_t(root.getD());

	String name = root.getName();
	if(name.length() > 0) {
		params.name = name;
	}

	String cmd = root.getCmd();
	if(cmd.length() > 0) {
		params.cmd = cmd;
	}

	String q = root.getQ();
	if(q.length() > 0) {
		if(q == "back")
			params.queue = QueuePolicy::Back;
		else if(q == "front")
			params.queue = QueuePolicy::Front;
		else if(q == "front_reset")
			params.queue = QueuePolicy::FrontReset;
		else if(q == "single")
			params.queue = QueuePolicy::Single;
		else {
			params.queue = QueuePolicy::Invalid;
		}
	}

	for(auto channel : root.channels) {
		String str = channel;
		if(str == "h") {
			params.channels.add(CtrlChannel::Hue);
		} else if(str == "s") {
			params.channels.add(CtrlChannel::Sat);
		} else if(str == "v") {
			params.channels.add(CtrlChannel::Val);
		} else if(str == "ct") {
			params.channels.add(CtrlChannel::ColorTemp);
		} else if(str == "r") {
			params.channels.add(CtrlChannel::Red);
		} else if(str == "g") {
			params.channels.add(CtrlChannel::Green);
		} else if(str == "b") {
			params.channels.add(CtrlChannel::Blue);
		} else if(str == "ww") {
			params.channels.add(CtrlChannel::WarmWhite);
		} else if(str == "cw") {
			params.channels.add(CtrlChannel::ColdWhite);
		}
	}
}

bool JsonProcessor::parseRequest(Stream& body, RequestParameters& params, std::vector<RequestParameters>* batch,
								 String& errorMsg)
{
	Jsonrpc::Root root(rpcCodec().db());
	auto update = root.update();
	if(!update) {
		errorMsg = F("internal error");
		return false;
	}

	auto fields = update.toCommandRequestFields();
	auto status = fields.importFromStream(ConfigDB::Json::format, body);
	if(!status) {
		// Unknown keys were silently ignored by the ArduinoJson parser; keep accepting them
		if(status.error != ConfigDB::Error::FormatError || status.code.formatError != ConfigDB::FormatError::NotInSchema) {
			errorMsg = F("Invalid JSON: ") + status.toString();
			return false;
		}
		debug_w(ANSI_COLOR_YELLOW "JsonProcessor::parseRequest: ignoring unknown field(s)" ANSI_COLOR_RESET);
	}

	parseCommandRequestFields(fields, params);
	if(batch != nullptr) {
		for(unsigned i = 0; i < fields.cmds.getItemCount(); ++i) {
			batch->emplace_back();
			parseCommandRequestFields(fields.cmds[i], batch->back());
		}
	}
	return true;
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
 * @return True if the request was successfully processed, false otherwise.
 */
bool JsonProcessor::onJsonRpc(const String& json)
{
	debug_d("JsonProcessor::onJsonRpc: %s\n", json.c_str());
	if(app.api) {
		String errorMsg;
		return app.api->dispatchJsonRpc(json, errorMsg, false);
	}

	JsonRpcMessageIn rpc(json);
	String msg;
	const char* method = rpc.getMethod();
	if(strcmp(method, "color") == 0) {
		return onColor(rpc.getParams(), msg, false);
	} else if(strcmp(method, "stop") == 0) {
		return onStop(rpc.getParams(), msg, false);
	} else if(strcmp(method, "blink") == 0) {
		return onBlink(rpc.getParams(), msg, false);
	} else if(strcmp(method, "skip") == 0) {
		return onSkip(rpc.getParams(), msg, false);
	} else if(strcmp(method, "pause") == 0) {
		return onPause(rpc.getParams(), msg, false);
	} else if(strcmp(method, "continue") == 0) {
		return onContinue(rpc.getParams(), msg, false);
	} else if(strcmp(method, "direct") == 0) {
		return onDirect(rpc.getParams(), msg, false);
	}

	return false;
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
	runSetOn(params);
	return true;
}

void JsonProcessor::runSetOn(const RequestParameters& params)
{
	app.rgbwwctrl.setOn(
		params.channels,
		params.direction,
		params.ramp,
		params.queue,
		params.requeue,
		params.name
	);
}

bool JsonProcessor::onSetOff(JsonObject root, String& msg, bool relay) {
	RequestParameters params;
	parseRequestParams(root, params);
	runSetOff(params);
	return true;
}

void JsonProcessor::runSetOff(RequestParameters& params)
{
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
}
