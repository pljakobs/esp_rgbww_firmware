#pragma once

#include <array>
#include <algorithm>
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

struct Channel {
	int bank = -1;
	uint8_t channel = 0;
};
using ChannelMap = std::array<Channel, 5>;

inline Status mapBoard(const std::vector<std::array<int, 5>>& lights, const std::vector<uint8_t>& boardPins,
					   std::size_t bankCapacity, std::size_t bankCount, std::vector<ChannelMap>& mappings)
{
	mappings.clear();
	if(bankCapacity == 0 || bankCount == 0 || bankCount > 2) return Status::InvalidConfiguration;
	if(boardPins.size() > bankCapacity * bankCount) return Status::CapacityExceeded;
	std::array<bool, 256> available{};
	for(auto gpio : boardPins) {
		if(available[gpio]) return Status::InvalidConfiguration;
		available[gpio] = true;
	}
	std::array<bool, 256> claimed{};
	for(const auto& pins : lights) {
		ChannelMap mapping{};
		for(size_t i = 0; i < pins.size(); ++i) {
			if(pins[i] < 0) continue;
			if(pins[i] > UINT8_MAX || !available[pins[i]] || claimed[pins[i]]) {
				mappings.clear();
				return Status::InvalidConfiguration;
			}
			claimed[pins[i]] = true;
			const auto position = std::find(boardPins.begin(), boardPins.end(), pins[i]) - boardPins.begin();
			mapping[i] = {int(position / bankCapacity), uint8_t(position % bankCapacity)};
		}
		mappings.push_back(mapping);
	}
	return Status::Ok;
}

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