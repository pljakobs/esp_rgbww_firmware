#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PwmChannelPlanner
{
enum class Status {
	Ok,
	InvalidConfiguration,
	CapacityExceeded,
};

struct Allocation {
	std::size_t primaryStart = 0;
	std::size_t secondaryStart = 0;
	std::size_t primaryCount = 0;
	std::size_t secondaryCount = 0;
	uint8_t secondaryChannelMask = 0;
};

inline Status plan(const std::vector<std::array<int, 5>>& channelPinsByLight, std::size_t bankCapacity,
				   std::size_t bankCount, std::vector<Allocation>& allocations)
{
	allocations.clear();
	if(bankCapacity == 0 || bankCount == 0 || bankCount > 2) {
		return Status::InvalidConfiguration;
	}

	std::size_t usedChannels[2] = {};
	allocations.reserve(channelPinsByLight.size());
	for(const auto& pins : channelPinsByLight) {
		Allocation allocation;
		allocation.primaryStart = usedChannels[0];
		allocation.secondaryStart = usedChannels[1];
		for(std::size_t channel = 0; channel < pins.size(); ++channel) {
			if(pins[channel] < 0) {
				continue;
			}
			if(usedChannels[0] < bankCapacity) {
				++usedChannels[0];
				++allocation.primaryCount;
			} else if(bankCount == 2 && usedChannels[1] < bankCapacity) {
				++usedChannels[1];
				++allocation.secondaryCount;
				allocation.secondaryChannelMask |= static_cast<uint8_t>(1U << channel);
			} else {
				allocations.clear();
				return Status::CapacityExceeded;
			}
		}
		allocations.push_back(allocation);
	}
	return Status::Ok;
}
} // namespace PwmChannelPlanner