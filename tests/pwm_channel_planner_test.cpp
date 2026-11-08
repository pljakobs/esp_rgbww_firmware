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
	testRgbwwLightCrossesBankBoundary();
	testDualBankCapacityAndOverflow();
	testSingleBankCapacity();
	testSparseChannelsKeepLogicalMaskPositions();
}