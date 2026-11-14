#include <SmingCore.h>
#include "as7341_extension.h"
#include <cstdlib>

void require(bool ok, const char* message)
{
	if(!ok) {
		Serial << "FAIL: " << message << endl;
		std::exit(1);
	}
}

void init()
{
	Serial.begin(115200);
	fileSetFileSystem(&IFS::Host::getFileSystem());
	{
		As7341Extension sensor;
		auto next = sensor.settings();
		require(next.enabled && next.intervalMs == 1000 && next.atime == 29 &&
				next.astep == 599 && next.gain == 6, "default settings");
		next.enabled = false;
		next.intervalMs = 200;
		next.atime = 255;
		next.astep = 65535;
		next.gain = 10;
		require(sensor.setSettings(next), "save without a detected sensor");
		require(!sensor.available(), "disabled sensor is unavailable");
		next.gain = 11;
		require(!sensor.setSettings(next), "reject invalid gain");
		next.gain = 10;
		next.intervalMs = 199;
		require(!sensor.setSettings(next), "reject too short interval");
		next.intervalMs = 3600001;
		require(!sensor.setSettings(next), "reject too long interval");
	}
	{
		As7341Extension sensor;
		const auto saved = sensor.settings();
		require(!saved.enabled && saved.intervalMs == 200 && saved.atime == 255 &&
				saved.astep == 65535 && saved.gain == 10, "settings survive reload and rejected updates");
	}
	Serial << "PASS: AS7341 settings persisted" << endl;
	std::exit(0);
}
