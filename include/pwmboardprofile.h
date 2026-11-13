#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace PwmBoardProfile
{
template <typename Configuration>
const char* load(const Configuration& configuration, std::vector<uint8_t>& pins)
{
	pins.clear();
	const auto& board = configuration.boardProfile;
	if(board.getId() != configuration.getBoardId() || board.getRevision() != configuration.getBoardRevision() ||
	   board.getId().length() == 0) {
		return "Selected board profile is missing or mismatched; select the board again";
	}
	if(board.getSoc() != configuration.getSoc()) {
		return "PWM board profile SoC does not match configuration";
	}
	for(auto pin : board.implementedPins) {
		bool pwm = false;
		for(auto function : pin.allowedFunctions) {
			pwm |= function == "light.pwm";
		}
		if(!pwm) continue;
		const auto gpio = pin.getGpio();
		if(std::find(pins.begin(), pins.end(), gpio) != pins.end()) {
			return "Duplicate board PWM GPIO";
		}
		for(auto reserved : board.reservedGpios) {
			if(reserved == gpio) return "Board PWM GPIO is reserved";
		}
		pins.push_back(gpio);
	}
	return pins.empty() ? "Selected board has no PWM outputs" : nullptr;
}
}
