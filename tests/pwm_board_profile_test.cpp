#include "pwmboardprofile.h"
#include <cassert>
#include <string>

struct Pin {
	uint8_t gpio;
	std::vector<std::string> allowedFunctions;
	uint8_t getGpio() const { return gpio; }
};

struct Board {
	std::string id = "board";
	unsigned revision = 1;
	std::string soc = "esp32c3";
	std::vector<Pin> implementedPins{{5, {"light.pwm"}}, {6, {"i2c.sda"}}, {0, {"light.pwm"}}};
	std::vector<uint8_t> reservedGpios;
	const std::string& getId() const { return id; }
	unsigned getRevision() const { return revision; }
	const std::string& getSoc() const { return soc; }
};

struct Configuration {
	Board boardProfile;
	std::string boardId = "board";
	unsigned revision = 1;
	std::string soc = "esp32c3";
	const std::string& getBoardId() const { return boardId; }
	unsigned getBoardRevision() const { return revision; }
	const std::string& getSoc() const { return soc; }
};

int main()
{
	Configuration configuration;
	std::vector<uint8_t> pins;
	assert(PwmBoardProfile::load(configuration, pins) == nullptr);
	assert((pins == std::vector<uint8_t>{5, 0}));
	auto rejects = [&](const Configuration& candidate) {
		assert(PwmBoardProfile::load(candidate, pins) != nullptr);
	};
	auto candidate = configuration;
	candidate.boardProfile.id.clear();
	rejects(candidate);
	candidate = configuration;
	candidate.boardProfile.id = "other";
	rejects(candidate);
	candidate = configuration;
	++candidate.boardProfile.revision;
	rejects(candidate);
	candidate = configuration;
	candidate.boardProfile.soc = "esp32";
	rejects(candidate);
	candidate = configuration;
	candidate.boardProfile.implementedPins.push_back({5, {"light.pwm"}});
	rejects(candidate);
	candidate = configuration;
	candidate.boardProfile.reservedGpios.push_back(5);
	rejects(candidate);
	candidate = configuration;
	candidate.boardProfile.implementedPins = {{6, {"i2c.sda"}}};
	rejects(candidate);
	configuration.boardProfile.implementedPins = {{0, {"light.pwm"}}, {5, {"light.pwm"}}};
	assert(PwmBoardProfile::load(configuration, pins) == nullptr);
	assert((pins == std::vector<uint8_t>{0, 5}));
}
