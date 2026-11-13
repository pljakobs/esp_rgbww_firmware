#include "pwmchannelplanner.h"

#include <cassert>
#include <vector>

using PinMap = std::array<int, 5>;
using PwmChannelPlanner::Allocation;
using PwmChannelPlanner::Status;

static PinMap fullLight(int firstPin)
{
	return {{firstPin, firstPin + 1, firstPin + 2, firstPin + 3, firstPin + 4}};
}

static void testRgbwwLightCrossesBankBoundary()
{
	const std::vector<PinMap> pins{fullLight(0), fullLight(5), fullLight(10)};
	std::vector<Allocation> allocations;

	assert(PwmChannelPlanner::plan(pins, 8, 2, allocations) == Status::Ok);
	assert(allocations.size() == 3);
	assert(allocations[0].primaryStart == 0 && allocations[0].primaryCount == 5);
	assert(allocations[0].secondaryCount == 0 && allocations[0].secondaryChannelMask == 0);
	assert(allocations[1].primaryStart == 5 && allocations[1].primaryCount == 3);
	assert(allocations[1].secondaryStart == 0 && allocations[1].secondaryCount == 2);
	assert(allocations[1].secondaryChannelMask == ((1U << 3) | (1U << 4)));
	assert(allocations[2].primaryStart == 8 && allocations[2].primaryCount == 0);
	assert(allocations[2].secondaryStart == 2 && allocations[2].secondaryCount == 5);
	assert(allocations[2].secondaryChannelMask == 0x1f);
}

static void testDualBankCapacityAndOverflow()
{
	std::vector<Allocation> allocations;
	const std::vector<PinMap> exactCapacity{fullLight(0), fullLight(5), fullLight(10), {{15, -1, -1, -1, -1}}};
	assert(PwmChannelPlanner::plan(exactCapacity, 8, 2, allocations) == Status::Ok);
	assert(allocations[2].primaryCount == 0 && allocations[2].secondaryCount == 5);
	assert(allocations[3].primaryCount == 0 && allocations[3].secondaryCount == 1);

	const std::vector<PinMap> overCapacity{fullLight(0), fullLight(5), fullLight(10), {{15, 16, -1, -1, -1}}};
	assert(PwmChannelPlanner::plan(overCapacity, 8, 2, allocations) == Status::CapacityExceeded);
	assert(allocations.empty());
}

static void testSingleBankCapacity()
{
	std::vector<Allocation> allocations;
	const std::vector<PinMap> withinCapacity{{{0, 1, 2, 3, 4}}, {{5, -1, -1, -1, -1}}};
	assert(PwmChannelPlanner::plan(withinCapacity, 6, 1, allocations) == Status::Ok);
	assert(allocations[1].primaryStart == 5 && allocations[1].primaryCount == 1);
	assert(allocations[1].secondaryCount == 0);

	const std::vector<PinMap> overCapacity{{{0, 1, 2, 3, 4}}, {{5, 6, -1, -1, -1}}};
	assert(PwmChannelPlanner::plan(overCapacity, 6, 1, allocations) == Status::CapacityExceeded);
	assert(allocations.empty());
}

static void testSparseChannelsKeepLogicalMaskPositions()
{
	const std::vector<PinMap> pins{{{20, -1, -1, 21, 22}}};
	std::vector<Allocation> allocations;

	assert(PwmChannelPlanner::plan(pins, 2, 2, allocations) == Status::Ok);
	assert(allocations[0].primaryCount == 2 && allocations[0].secondaryCount == 1);
	assert(allocations[0].secondaryChannelMask == (1U << 4));
}

int main()
{
	// Physical placement follows board order, never light order/count.
	const std::vector<uint8_t> board{27, 26, 25, 23, 22, 21, 19, 18, 17, 16, 15, 14, 13, 12, 4};
	std::vector<PwmChannelPlanner::ChannelMap> mappings;
	const std::vector<PinMap> reordered{{{17, -1, -1, 18, -1}}, {{27, -1, -1, -1, -1}}};
	assert(PwmChannelPlanner::mapBoard(reordered, board, 8, 2, mappings) == Status::Ok);
	assert(mappings[0][0].bank == 1 && mappings[0][0].channel == 0);
	assert(mappings[0][3].bank == 0 && mappings[0][3].channel == 7);
	assert(mappings[0][1].bank == -1);
	assert(mappings[1][0].bank == 0 && mappings[1][0].channel == 0);
	assert(PwmChannelPlanner::mapBoard({reordered[0]}, board, 8, 2, mappings) == Status::Ok);
	assert(mappings[0][0].bank == 1 && mappings[0][0].channel == 0);
	assert(PwmChannelPlanner::mapBoard({}, board, 8, 2, mappings) == Status::Ok);
	assert(PwmChannelPlanner::mapBoard({{{99, -1, -1, -1, -1}}}, board, 8, 2, mappings) == Status::InvalidConfiguration);
	assert(PwmChannelPlanner::mapBoard({reordered[0], reordered[0]}, board, 8, 2, mappings) == Status::InvalidConfiguration);
	assert(mappings.empty());
	assert(PwmChannelPlanner::mapBoard({}, {4, 4}, 8, 2, mappings) == Status::InvalidConfiguration);
	assert(PwmChannelPlanner::mapBoard({}, board, 6, 1, mappings) == Status::CapacityExceeded);
	testRgbwwLightCrossesBankBoundary();
	testDualBankCapacityAndOverflow();
	testSingleBankCapacity();
	testSparseChannelsKeepLogicalMaskPositions();
}