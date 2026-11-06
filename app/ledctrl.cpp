
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

#include <Wiring/WVector.h>
#include <Wiring/SplitString.h>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace
{
bool selectLightType(const String& type, unsigned pinCount, LightType& lightType)
{
	unsigned expectedPins = 0;
	if(type == F("RGBWWCW")) {
		lightType = LightType::RGBWWCW;
		expectedPins = 5;
	} else if(type == F("RGBWW")) {
		lightType = LightType::RGBWW;
		expectedPins = 4;
	} else if(type == F("RGBCW")) {
		lightType = LightType::RGBCW;
		expectedPins = 4;
	} else if(type == F("RGB")) {
		lightType = LightType::RGB;
		expectedPins = 3;
	} else if(type == F("WWCW")) {
		lightType = LightType::WWCW;
		expectedPins = 2;
	} else if(type == F("WW")) {
		lightType = LightType::WW;
		expectedPins = 1;
	} else if(type == F("CW")) {
		lightType = LightType::CW;
		expectedPins = 1;
	} else if(type == F("SINGLE")) {
		lightType = LightType::SINGLE;
		expectedPins = 1;
	} else {
		return false;
	}
	return pinCount == expectedPins;
}

struct ResolvedLight {
	String id;
	String name;
	LightType type;
	std::array<int, 5> pins{{-1, -1, -1, -1, -1}};
};
template <typename Hardware, typename Board>
bool claimBoardPin(const Hardware& hardware, const Board& board, const String& soc, const String& id,
				   const String& function, std::array<bool, 256>& claimed, int& gpio)
{
	for(auto pin : board.implementedPins) {
		if(pin.getId() != id) {
			continue;
		}
		bool allowed = false;
		for(auto supported : pin.allowedFunctions) {
			allowed = allowed || supported == function;
		}
		for(auto restriction : pin.restrictions) {
			if(restriction == F("electrical_behavior_unqualified") || restriction == F("driver_unimplemented")) {
				return false;
			}
		}
		gpio = pin.getGpio();
		if(!allowed || claimed[gpio]) {
			return false;
		}
		for(auto reserved : board.reservedGpios) {
			if(reserved == gpio) {
				return false;
			}
		}
		for(auto available : hardware.availablePins) {
			if(available.getSoc() != soc) {
				continue;
			}
			for(auto capability : available.pins) {
				if(capability.getGpio() != gpio) {
					continue;
				}
				const uint8_t capabilities = capability.getCapability();
				if(capabilities & CAPA_RESERVED) {
					return false;
				}
				if(function == F("light.pwm")) {
					if(!(capabilities & CAPA_OUTPUT) || !(capabilities & CAPA_PWM)) {
						return false;
					}
				} else if(!(capabilities & CAPA_INPUT) ||
						  ((function == F("i2c.sda") || function == F("i2c.scl")) && !(capabilities & CAPA_OUTPUT))) {
					return false;
				}
				claimed[gpio] = true;
				return true;
			}
		}
		return false;
	}
	return false;
}

template <typename Hardware>
bool resolveConfiguredLights(const Hardware& hardware, const String& soc, unsigned capacity,
							 std::vector<ResolvedLight>& result, String& error)
{
	result.clear();
	const auto& configuration = hardware.configuration;
	if(configuration.lights.getItemCount() == 0 && configuration.buttons.getItemCount() == 0 &&
	   configuration.buses.getItemCount() == 0 && configuration.system.getClearConfigPin().length() == 0) {
		return true;
	}
	unsigned boardIndex = hardware.boards.getItemCount();
	for(unsigned index = 0; index < hardware.boards.getItemCount(); ++index) {
		auto candidate = hardware.boards[index];
		if(candidate.getId() == configuration.getBoardId() &&
		   candidate.getRevision() == configuration.getBoardRevision() &&
		   (candidate.getSoc() == soc || soc == F("host"))) {
			boardIndex = index;
			break;
		}
	}
	if(boardIndex == hardware.boards.getItemCount()) {
		error = F("Configured board/revision is unavailable for this SoC");
		return false;
	}
	auto board = hardware.boards[boardIndex];
	const String boardSoc = board.getSoc();
	std::array<bool, 256> claimed{};
	int gpio = -1;
	const String clearPin = configuration.system.getClearConfigPin();
	if(clearPin.length() && !claimBoardPin(hardware, board, boardSoc, clearPin, F("system.clear_config"), claimed, gpio)) {
		error = F("Invalid or conflicting clear input");
		return false;
	}
	for(auto button : configuration.buttons) {
		if(!claimBoardPin(hardware, board, boardSoc, button.getPin(), F("button.input"), claimed, gpio)) {
			error = F("Invalid or conflicting button input");
			return false;
		}
	}
	bool displayBusConfigured = false;
	for(auto bus : configuration.buses) {
		const bool usesDisplayBus = bus.getSda() == F("I2C_SDA") && bus.getScl() == F("I2C_SCL");
		displayBusConfigured = displayBusConfigured || usesDisplayBus;
		if(!claimBoardPin(hardware, board, boardSoc, bus.getSda(), F("i2c.sda"), claimed, gpio) ||
		   !claimBoardPin(hardware, board, boardSoc, bus.getScl(), F("i2c.scl"), claimed, gpio)) {
			error = F("Invalid or conflicting I2C pins");
			return false;
		}
	}
#if STATUS_DISPLAY
	if(!displayBusConfigured) {
		int displaySda = -1;
		int displayScl = -1;
		if(!claimBoardPin(hardware, board, boardSoc, F("I2C_SDA"), F("i2c.sda"), claimed, displaySda) ||
		   !claimBoardPin(hardware, board, boardSoc, F("I2C_SCL"), F("i2c.scl"), claimed, displayScl) ||
		   displaySda != STATUS_DISPLAY_SDA || displayScl != STATUS_DISPLAY_SCL) {
			error = F("Compiled display I2C pins are unavailable or conflicting");
			return false;
		}
	}
#endif
	unsigned channelCount = 0;
	for(auto configured : configuration.lights) {
		ResolvedLight resolved;
		resolved.id = configured.getId();
		resolved.name = configured.getName();
		if(!resolved.id.length() || std::any_of(result.begin(), result.end(), [&](const ResolvedLight& other) {
			   return other.id == resolved.id;
		   })) {
			error = F("Duplicate or empty virtual-light ID");
			return false;
		}
		const unsigned pinCount = configured.pins.getItemCount();
		if(pinCount > 5 || !selectLightType(configured.getType(), pinCount, resolved.type)) {
			error = F("Invalid light type or pin count");
			return false;
		}
		std::array<int, 5> channelPins{};
		channelPins.fill(-1);
		std::array<unsigned, 5> channelOrder{{0, 1, 2, 3, 4}};
		switch(resolved.type) {
		case LightType::RGB:
			break;
		case LightType::RGBWW:
			break;
		case LightType::RGBCW:
			channelOrder = {{0, 1, 2, 4, 0}};
			break;
		case LightType::RGBWWCW:
			break;
		case LightType::WWCW:
			channelOrder = {{3, 4, 0, 0, 0}};
			break;
		case LightType::WW:
		case LightType::SINGLE:
			channelOrder = {{3, 0, 0, 0, 0}};
			break;
		case LightType::CW:
			channelOrder = {{4, 0, 0, 0, 0}};
			break;
		}
		for(unsigned pinIndex = 0; pinIndex < pinCount; ++pinIndex) {
			if(!claimBoardPin(hardware, board, boardSoc, configured.pins[pinIndex], F("light.pwm"), claimed, gpio)) {
				error = F("Unknown, unavailable or conflicting PWM output");
				return false;
			}
			const unsigned channel = channelOrder[pinIndex];
			if(channelPins[channel] >= 0) {
				error = F("Duplicate light channel assignment");
				return false;
			}
			channelPins[channel] = gpio;
			if(++channelCount > capacity) {
				error = F("PWM channel capacity exceeded");
				return false;
			}
		}
		resolved.pins = channelPins;
		result.push_back(std::move(resolved));
	}
	return true;
}

template <typename Color>
void configureLightColor(RGBWWLed& light, const Color& color)
{
	light.colorutils.setBrightnessCorrection(color.brightness.getRed(), color.brightness.getGreen(),
											color.brightness.getBlue(), color.brightness.getWw(), color.brightness.getCw());
	light.colorutils.setHSVcorrection(color.hsv.getRed(), color.hsv.getYellow(), color.hsv.getGreen(),
									 color.hsv.getCyan(), color.hsv.getBlue(), color.hsv.getMagenta());
	light.colorutils.setHSVmodel((RGBWW_HSVMODEL)color.hsv.getModel());
	light.colorutils.setWhiteTemperature(color.colortemp.getWw(), color.colortemp.getCw());
}
}

APPLedCtrl::~APPLedCtrl()
{
	_reloadLifetime.reset();
	delete _stepSync;
	_stepSync = nullptr;
}


bool APPLedCtrl::resolveInputs(HardwareInputs& inputs, String& error) const
{
	inputs = HardwareInputs{};
	AppConfig::Hardware hardware(*app.cfg);
	unsigned capacity = 16;
#ifdef ARCH_ESP32
	capacity = LEDC_CHANNEL_MAX;
#endif
	std::vector<ResolvedLight> lights;
	if(!resolveConfiguredLights(hardware, String(SOC), capacity, lights, error)) {
		return false;
	}
	const auto& configuration = hardware.configuration;
	if(configuration.buttons.getItemCount() == 0 && configuration.buses.getItemCount() == 0 &&
	   configuration.system.getClearConfigPin().length() == 0) {
#if STATUS_DISPLAY
		inputs.i2cEnabled = true;
		inputs.sda = STATUS_DISPLAY_SDA;
		inputs.scl = STATUS_DISPLAY_SCL;
		inputs.frequency = 100000;
#endif
		return true;
	}
	unsigned boardIndex = hardware.boards.getItemCount();
	for(unsigned index = 0; index < hardware.boards.getItemCount(); ++index) {
		auto board = hardware.boards[index];
		if(board.getId() == configuration.getBoardId() && board.getRevision() == configuration.getBoardRevision()) {
			boardIndex = index;
			break;
		}
	}
	if(boardIndex == hardware.boards.getItemCount()) {
		error = F("Configured input board is unavailable");
		return false;
	}
	auto board = hardware.boards[boardIndex];
	auto resolveInput = [&](const String& pinId, ConfiguredInput& input) {
		for(auto pin : board.implementedPins) {
			if(pin.getId() != pinId) {
				continue;
			}
			input.gpio = pin.getGpio();
			input.activeLow = pin.getActiveLow();
			if(pin.getExternalPull() == 0) {
				for(auto available : hardware.availablePins) {
					if(available.getSoc() != board.getSoc()) {
						continue;
					}
					for(auto capability : available.pins) {
						if(capability.getGpio() == input.gpio) {
								const uint8_t capabilities = capability.getCapability();
								input.internalPullUp = input.activeLow && (capabilities & CAPA_PULL_UP);
								input.internalPullDown = !input.activeLow && (capabilities & CAPA_PULL_DOWN);
						}
					}
				}
			}
			return true;
		}
		return false;
	};
	const String clearId = configuration.system.getClearConfigPin();
	if(clearId.length()) {
		inputs.clear.id = clearId;
		if(!resolveInput(clearId, inputs.clear)) {
			error = F("Clear input is unavailable");
			return false;
		}
	}
	for(auto button : configuration.buttons) {
		if(!button.getEnabled()) {
			continue;
		}
		ConfiguredInput input;
		input.id = button.getId();
		input.debounceMs = button.getDebounceMs();
		if(!input.id.length() || std::any_of(inputs.buttons.begin(), inputs.buttons.end(), [&](const ConfiguredInput& other) {
			   return other.id == input.id;
		   }) || !resolveInput(button.getPin(), input)) {
			error = F("Duplicate or invalid configured button");
			return false;
		}
		inputs.buttons.push_back(std::move(input));
	}
	for(auto bus : configuration.buses) {
		if(!bus.getEnabled()) {
			continue;
		}
		if(inputs.i2cEnabled || bus.getType() != F("i2c") || !bus.getId().length() ||
		   bus.getFrequency() < 10000 || bus.getFrequency() > 400000) {
			error = F("Only one valid I2C bus may be enabled");
			return false;
		}
		ConfiguredInput sda, scl;
		if(!resolveInput(bus.getSda(), sda) || !resolveInput(bus.getScl(), scl)) {
			error = F("I2C pin mapping is unavailable");
			return false;
		}
		inputs.i2cEnabled = true;
		inputs.sda = sda.gpio;
		inputs.scl = scl.gpio;
		inputs.frequency = bus.getFrequency();
	}
#if STATUS_DISPLAY
	if(!inputs.i2cEnabled) {
		inputs.i2cEnabled = true;
		inputs.sda = STATUS_DISPLAY_SDA;
		inputs.scl = STATUS_DISPLAY_SCL;
		inputs.frequency = 100000;
	}
	if(inputs.i2cEnabled && (inputs.sda != STATUS_DISPLAY_SDA || inputs.scl != STATUS_DISPLAY_SCL || inputs.frequency != 100000)) {
		error = F("I2C settings conflict with the compiled status display");
		return false;
	}
#endif
	return true;
}

void APPLedCtrl::init()
{
	delete _stepSync;
	_stepSync = new StepSync();
	reconfigure();
	String error;
	if(!reload(error)) {
		cdebug_e(APPLEDCTRL, "Light initialization failed: %s", error.c_str());
	}
	if(!_watchingConfiguration) {
		_watchingConfiguration = true;
		std::weak_ptr<uint8_t> lifetime = _reloadLifetime;
		AppConfig::Hardware::onCommit(*app.cfg, [this, lifetime](AppConfig::HardwareUpdater) {
			if(!lifetime.expired()) {
				requestReload();
			}
		});
	}
}

String APPLedCtrl::configurationSignature() const
{
	AppConfig::Hardware hardware(*app.cfg);
	MemoryDataStream stream;
	hardware.configuration.exportToStream(ConfigDB::Json::format, stream);
	hardware.pwm.exportToStream(ConfigDB::Json::format, stream);
	for(auto board : hardware.boards) {
		if(board.getId() == hardware.configuration.getBoardId() &&
		   board.getRevision() == hardware.configuration.getBoardRevision()) {
			board.exportToStream(ConfigDB::Json::format, stream);
			break;
		}
	}
	return stream.readString(stream.available());
}

void APPLedCtrl::requestReload()
{
	if(_reloadQueued) {
		return;
	}
	_reloadQueued = true;
	std::weak_ptr<uint8_t> lifetime = _reloadLifetime;
	System.queueCallback([this, lifetime]() {
		if(lifetime.expired()) {
			return;
		}
		_reloadQueued = false;
		if(configurationSignature() == _activeConfiguration) {
			return;
		}
		String error;
		if(!reload(error)) {
			if(error == F("PWM callbacks pending")) {
				requestReload();
			} else {
				cdebug_e(APPLEDCTRL, "Light reload rejected: %s", error.c_str());
			}
		}
	});
}

bool APPLedCtrl::reload(String& error)
{
	HardwareInputs inputs;
	if(!resolveInputs(inputs, error)) {
		return false;
	}
	AppConfig::Hardware hardware(*app.cfg);
	unsigned capacity = 16;
#ifdef ARCH_ESP32
	capacity = LEDC_CHANNEL_MAX;
#endif
	std::vector<ResolvedLight> resolved;
	if(!resolveConfiguredLights(hardware, String(SOC), capacity, resolved, error)) {
		return false;
	}
#ifdef ARCH_ESP32
	for(auto& light : app.lights) {
		if(light.led->getPwmOutput() != nullptr && light.led->getPwmOutput()->hasPendingCallbacks()) {
			error = F("PWM callbacks pending");
			return false;
		}
	}
#endif
	if(resolved.empty()) {
		for(auto& light : app.lights) {
			light.led->clearAnimationQueue();
			light.led->getPwmOutput()->setOutput(0, 0, 0, 0, 0);
		}
		app.lights.clear();
		_activeConfiguration = configurationSignature();
		cdebug_i(APPLEDCTRL, "No virtual lights configured");
		return true;
	}
#ifdef ARCH_ESP32
	const auto& pwm = hardware.pwm;
	if(pwm.timer.getFrequency() <= 0) {
		error = F("Invalid PWM frequency");
		return false;
	}
	Esp32HardwarePwm::Config pwmConfig;
	pwmConfig.timer.frequency = pwm.timer.getFrequency();
	pwmConfig.timer.resolution = (ledc_timer_bit_t)pwm.timer.getResolution();
	pwmConfig.timer.timer_num = (ledc_timer_t)pwm.timer.getNumber();
	const auto speedMode = pwm.timer.getSpeedMode();
#ifdef SOC_ESP32
	if(speedMode == AppConfig::ContainedHardware::ContainedPwm::TimerSpeedMode::HIGHSPEED) {
		pwmConfig.timer.speed_mode = LEDC_HIGH_SPEED_MODE;
	} else
#endif
	if(speedMode == AppConfig::ContainedHardware::ContainedPwm::TimerSpeedMode::LOWSPEED) {
		pwmConfig.timer.speed_mode = LEDC_LOW_SPEED_MODE;
	} else {
		error = F("Unsupported PWM speed mode");
		return false;
	}
	pwmConfig.spreadSpectrum.mode = pwm.spreadSpectrum.getMode() ==
		AppConfig::ContainedHardware::ContainedPwm::SpreadSpectrumMode::ON
		? Esp32HardwarePwm::SpreadSpectrumMode::ON : Esp32HardwarePwm::SpreadSpectrumMode::OFF;
	pwmConfig.spreadSpectrum.WidthPercent = pwm.spreadSpectrum.getWidth();
	pwmConfig.spreadSpectrum.Subsampling = pwm.spreadSpectrum.getSubsampling();
	pwmConfig.phaseShift.mode = pwm.phaseShift.getMode() ==
		AppConfig::ContainedHardware::ContainedPwm::PhaseShiftMode::ON
		? Esp32HardwarePwm::PhaseShiftMode::AUTO : Esp32HardwarePwm::PhaseShiftMode::OFF;
#endif
	std::vector<VirtualLight> staged;
	staged.reserve(resolved.size());
	for(const auto& light : resolved) {
		std::unique_ptr<RGBWWLed> instance(new RGBWWLed());
		if(!instance) {
			error = F("Unable to allocate virtual light");
			return false;
		}
		staged.push_back({light.id, light.name, std::move(instance)});
	}
	for(auto& light : app.lights) {
		light.led->clearAnimationQueue();
		light.led->getPwmOutput()->setOutput(0, 0, 0, 0, 0);
	}
	app.lights.clear();
#ifdef ARCH_ESP32
	unsigned channelStart = 0;
#endif
	for(unsigned index = 0; index < resolved.size(); ++index) {
		const auto& light = resolved[index];
		const auto& pins = light.pins;
#ifdef ARCH_ESP32
		pwmConfig.channelStart = (ledc_channel_t)channelStart;
		staged[index].led->init(pins[0], pins[1], pins[2], pins[3], pins[4], light.type, pwmConfig);
		channelStart += std::count_if(pins.begin(), pins.end(), [](int gpio) { return gpio >= 0; });
#else
		staged[index].led->init(pins[0], pins[1], pins[2], pins[3], pins[4], light.type, PWM_FREQUENCY);
#endif
	}
	app.lights.swap(staged);
	setup();
	_activeConfiguration = configurationSignature();
	cdebug_i(APPLEDCTRL, "Initialized %u virtual lights", unsigned(app.lights.size()));
	return true;
}


/**
 * @brief read local configuration from ConfigDB
 * 
 */
void APPLedCtrl::reconfigure()
{
	cdebug_i(APPLEDCTRL, "APPLedCtrl::reconfigure: " ANSI_COLOR_BLUE "APPLedCtrl::reconfigure" ANSI_COLOR_RESET);
	{
		AppConfig::Sync sync(*app.cfg);
		clockMaster = sync.getClockMasterEnabled();
		clockMasterInterval = sync.getClockMasterInterval();
		colorMaster = sync.getColorMasterEnabled();
		colorMasterInterval = sync.getColorMasterIntervalMs();

	} // end AppConfig::Sync context
	{
		AppConfig::Root config(*app.cfg);
		transFinInterval = config.events.getTransFinIntervalMs();
		colorMinInterval = config.events.getColorMinIntervalMs();
	} //close configdb root context
	{
		AppConfig::Color color(*app.cfg);
		startupColorLast = (color.getStartupColor() == "last");
	} //close configdb context for color
}
/**
 * @brief Initializes the LED controller.
 * 
 * This function sets up the LED controller by configuring the brightness correction,
 * HSV correction, color mode, HSV model, and white temperature based on the 
 * configuration settings stored in the `app` object.
 */
void APPLedCtrl::setup()
{
	AppConfig::Hardware hardware(*app.cfg);
	for(auto configured : hardware.configuration.lights) {
		for(auto& light : app.lights) {
			if(light.id == configured.getId()) {
				configureLightColor(*light.led, configured.color);
				break;
			}
		}
	}
}

/**
 * @brief Publishes the current state of the LED controller to the event server.
 * 
 * This function checks if the event server is enabled and updates the clients accordingly.
 * It publishes the current output and color information to the event server.
 */
void APPLedCtrl::publishToEventServer()
{
	if(!app.eventserver.isEnabled()) {
		cdebug_i(APPLEDCTRL, "APPLedCtrl::publishToEventServer: " ANSI_COLOR_BLUE "APPLEDCtrl - eventserver is disabled" ANSI_COLOR_RESET);
		return;
	}
	HSVCT currentHsv;
	const HSVCT* pHsv = nullptr;
	if(_mode == ColorMode::Hsv) {
		currentHsv = getCurrentColor();
		pHsv = &currentHsv;
	}

	app.eventserver.publishCurrentState(getCurrentOutput(), pHsv);
}

/**
 * @brief Publishes the current LED color or output to MQTT.
 * 
 * This function is responsible for publishing the current LED color or output to MQTT.
 * It checks if the color master is enabled and based on the current color mode, it publishes
 * the corresponding data to the MQTT broker.
 * 
 * @note This function does nothing if the color master is disabled.
 */
void APPLedCtrl::publishToMqtt()
{
	bool mqttEnabled;
	{
		AppConfig::Network network(*app.cfg);
		mqttEnabled = network.mqtt.getEnabled();
	}
	if(mqttEnabled) {
		if(colorMaster) {
			switch(_mode) {
			case ColorMode::Hsv:
				app.mqttclient.publishCurrentHsv(getCurrentColor());
				break;
			case ColorMode::Raw:
				app.mqttclient.publishCurrentRaw(getCurrentOutput());
				break;
			}
		}
	}
}

void APPLedCtrl::updateLedCb(void* pTimerArg)
{
	APPLedCtrl* pThis = static_cast<APPLedCtrl*>(pTimerArg);
	pThis->updateLed();
}

/**
 * @brief Updates the LED state and performs various actions based on the configuration settings.
 *
 * This function is responsible for updating the LED state and performing actions such as publishing clock values,
 * publishing to MQTT, checking for stable color state, and publishing finished step animations.
 * The function takes into account the configuration settings for synchronization, events, and intervals.
 * It also checks if the animation has finished and if the color or transition interval has elapsed.
 *
 * @note This function relies on the configuration settings provided by the `app` object.
 */
void APPLedCtrl::updateLed()
{
	_ledTimer.startOnce();
	for(auto& light : app.lights) {
		light.led->show();
	}
	++_stepCounter;
}

/**
 * @brief Checks if the current color state is stable and saves it if necessary.
 *
 * This function checks if the current color state is stable by comparing it with the previous color state.
 * If the current color state is the same as the previous color state, the number of stable color steps is incremented.
 * If the current color state is different from the previous color state, the previous color state is updated and the number of stable color steps is reset.
 * If the number of stable color steps reaches a certain threshold, the current color state is saved.
 */
void APPLedCtrl::checkStableColorState()
{
	if(!startupColorLast)
		return;

	if(_prevColor == getCurrentColor()) {
		++_numStableColorSteps;
	} else {
		_prevColor = getCurrentColor();
		_numStableColorSteps = 0;
	}

	// save if color was stable for _saveAfterStableColorMs
	if(abs(_saveAfterStableColorMs - (_numStableColorSteps * RGBWW_MINTIMEDIFF)) <= (uint32_t)(RGBWW_MINTIMEDIFF / 2))
		colorSave();
}

/**
 * @brief Publishes the finished step animations.
 * 
 * This function publishes the finished step animations to the MQTT client and event server.
 * It iterates through the list of step animations and publishes each animation's name and requeued status.
 * After publishing, the list of step animations is cleared.
 */
void APPLedCtrl::publishFinishedStepAnimations()
{
	for(unsigned int i = 0; i < _stepFinishedAnimations.count(); i++) {
		const String& name = _stepFinishedAnimations.keyAt(i);
		const bool requeued = _stepFinishedAnimations.valueAt(i);
		app.mqttclient.publishTransitionFinished(name, requeued);
		app.eventserver.publishTransitionFinished(name, requeued);
	}
	_stepFinishedAnimations.clear();
}

/**
 * @brief Resets the master clock and updates the timer interval.
 * 
 * This function is called when the master clock is reset. It resets the step synchronization
 * and updates the timer interval accordingly. After updating the timer interval, it publishes
 * the status.
 */
void APPLedCtrl::onMasterClockReset()
{
	_timerInterval = _stepSync->reset();
	publishStatus();
}

/**
 * @brief Handles the master clock event and updates the timer interval for LED control.
 * 
 * This function is called when the master clock event occurs. It calculates the new timer interval
 * based on the current step counter and the number of steps in the master clock. The timer interval
 * is then limited to ensure it falls within a safe range. Finally, the timer interval is set and the
 * status is published.
 * 
 * @param stepsMaster The number of steps in the master clock.
 */
void APPLedCtrl::onMasterClock(uint32_t stepsMaster)
{
	_timerInterval = _stepSync->onMasterClock(_stepCounter, stepsMaster);

	// limit interval to sane values (just for safety)
	_timerInterval = std::min(std::max(_timerInterval, static_cast<uint32_t>(RGBWW_MINTIMEDIFF_US / 2u)),
							  static_cast<uint32_t>(RGBWW_MINTIMEDIFF_US * 1.5));
	_ledTimer.setIntervalUs(_timerInterval);
	publishStatus();
}

void APPLedCtrl::publishStatus()
{
	app.eventserver.publishClockSlaveStatus(_stepSync->getCatchupOffset(), _timerInterval);
	app.mqttclient.publishClockSlaveOffset(_stepSync->getCatchupOffset());
	app.mqttclient.publishClockInterval(_timerInterval);
}

void APPLedCtrl::start()
{
	cdebug_i(APPLEDCTRL, "APPLedCtrl::start: " ANSI_COLOR_BLUE "APPLedCtrl::start" ANSI_COLOR_RESET);

	_ledTimer.setCallback(APPLedCtrl::updateLedCb, this);
	_ledTimer.setIntervalMs(_timerInterval);
	cdebug_i(APPLEDCTRL, "APPLedCtrl::start: " ANSI_COLOR_BLUE "_timerInterval" ANSI_COLOR_RESET, _timerInterval);
	_ledTimer.startOnce();
}

void APPLedCtrl::stop()
{
	cdebug_i(APPLEDCTRL, "APPLedCtrl::stop: " ANSI_COLOR_BLUE "APPLedCtrl::stop" ANSI_COLOR_RESET);
	_ledTimer.stop();
}

void APPLedCtrl::colorSave()
{
	AppData::Root data(*app.data);
	{
		auto update = data.update();
		float h, s, v;
		int ct;
		HSVCT c = app.rgbwwctrl.getCurrentColor();
		c.asRadian(h, s, v, ct);
		cdebug_i(APPLEDCTRL, "APPLedCtrl::colorSave: " ANSI_COLOR_BLUE "saving color H: " ANSI_COLOR_CYAN "%f" ANSI_COLOR_BLUE " | S: " ANSI_COLOR_CYAN "%f" ANSI_COLOR_BLUE " | V: " ANSI_COLOR_CYAN "%f" ANSI_COLOR_BLUE " | CT: " ANSI_COLOR_CYAN "%f" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, h, s, v, ct);
		update.lastColor.setH(h);
		update.lastColor.setS(s);
		update.lastColor.setV(v);
		update.lastColor.setCt(ct);
	}
}

void APPLedCtrl::colorReset()
{
	cdebug_i(APPLEDCTRL, "APPLedCtrl::colorReset: " ANSI_COLOR_BLUE "APPLedCtrl::colorReset" ANSI_COLOR_RESET);
	AppData::Root data(*app.data);
	{
		auto update = data.update();
		update.lastColor.setH(0);
		update.lastColor.setS(0);
		update.lastColor.setV(0);
		update.lastColor.setCt(0);
	}
}

void APPLedCtrl::onAnimationFinished(const String& name, bool requeued)
{
	cdebug_d(APPLEDCTRL, "APPLedCtrl::onAnimationFinished: " "%s", name.c_str());

	if(name.length() > 0) {
		_stepFinishedAnimations[name] = requeued;
	}
}

void APPLedCtrl::toggle()
{
	static const int toggleFadeTime = 300;
	switch(_mode) {
	case ColorMode::Hsv: {
		HSVCT current = getCurrentColor();
		if(current.v > 0) {
			cdebug_d(APPLEDCTRL, "APPLedCtrl::toggle: " "off");
			_lastHsvct = current;
			current.v = 0;
			fadeHSV(_lastHsvct, current, toggleFadeTime);
		} else {
			cdebug_d(APPLEDCTRL, "APPLedCtrl::toggle: " "on");
			if(_lastHsvct.v == 0)
				_lastHsvct.v = 100; // we were off before but force some light
			fadeHSV(current, _lastHsvct, toggleFadeTime);
		}
		break;
	}
	case ColorMode::Raw: {
		ChannelOutput current = getCurrentOutput();
		if(current.isOn()) {
			cdebug_d(APPLEDCTRL, "APPLedCtrl::toggle: " "off");
			_lastOutput = current;
			current.r = current.g = current.b = 0;
			current.ww = current.cw = 0;
			fadeRAW(_lastOutput, current, toggleFadeTime);
		} else {
			cdebug_d(APPLEDCTRL, "APPLedCtrl::toggle: " "on");
			if(!_lastOutput.isOn())
				_lastOutput.r = _lastOutput.g = _lastOutput.b = _lastOutput.cw = _lastOutput.ww =
					255; // was off before but force light

			fadeRAW(current, _lastOutput, toggleFadeTime);
		}
	}
	}
}


// SetOn/SetOff implementation (refactored to use plain arguments)
void APPLedCtrl::setOn(const RGBWWLed::ChannelList& channels, int direction, const RampTimeOrSpeed& ramp, QueuePolicy queue, bool requeue, const String& name)
{
	switch(_mode) {
	case ColorMode::Hsv: {
		HSVCT current = getCurrentColor();
		HSVCT target = _lastHsvct;
		// If v==0, restore last nonzero HSV (toggle logic)
		if (target.v == 0) 
			target.v = 100; // fallback if last was off
		// For HSV mode, direction/ramp/queue/requeue/name are used, but channels are ignored
		fadeHSV(current, target, ramp, direction, queue, requeue, name);
		break;
	}
	case ColorMode::Raw: {
		ChannelOutput current = getCurrentOutput();
		ChannelOutput target = current;
		// If channels specified, only set those to last or 255, else all
		if (!channels.isEmpty()) {
			for (auto ch : channels) {
				switch (ch) {
					case CtrlChannel::Red: target.r = 255; break;
					case CtrlChannel::Green: target.g = 255; break;
					case CtrlChannel::Blue: target.b = 255; break;
					case CtrlChannel::WarmWhite: target.ww = 255; break;
					case CtrlChannel::ColdWhite: target.cw = 255; break;
					default: break;
				}
			}
		} else {
			target.r = target.g = target.b = target.cw = target.ww = 255;
		}
		fadeRAW(current, target, ramp, queue, requeue, name);
		break;
	}
	default:
		break;
	}
}

void APPLedCtrl::setOff(const RGBWWLed::ChannelList& channels, int direction, const RampTimeOrSpeed& ramp, QueuePolicy queue, bool requeue, const String& name)
{
    switch(_mode) {
    case ColorMode::Hsv: {
        HSVCT current = getCurrentColor();
        HSVCT target = current;
		_lastHsvct = current; // save current before turning off
        target.v = 0;
        fadeHSV(current, target, ramp, direction, queue, requeue, name);
        break;
    }
    case ColorMode::Raw: {
        ChannelOutput current = getCurrentOutput();
        ChannelOutput target = current;
        if (!channels.isEmpty()) {
            for (auto ch : channels) {
                switch (ch) {
                    case CtrlChannel::Red: target.r = 0; break;
                    case CtrlChannel::Green: target.g = 0; break;
                    case CtrlChannel::Blue: target.b = 0; break;
                    case CtrlChannel::WarmWhite: target.ww = 0; break;
                    case CtrlChannel::ColdWhite: target.cw = 0; break;
                    default: break;
                }
            }
        } else {
            target.r = target.g = target.b = target.cw = target.ww = 0;
        }
        fadeRAW(current, target, ramp, queue, requeue, name);
        break;
    }
    default:
        break;
    }
}