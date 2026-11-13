#include <SmingCore.h>
#include <ConfigDB/Json/Format.h>
#include <Data/Stream/MemoryDataStream.h>
#include <app-config.h>
#include <pwmboardprofile.h>
#include <cstdlib>

namespace
{
void require(bool condition, const char* message)
{
	if(!condition) {
		Serial << "FAIL: " << message << endl;
		std::exit(1);
	}
}

void check(AppConfig& database)
{
	AppConfig::Hardware hardware(database);
	const auto& configuration = hardware.configuration;
	require(configuration.getBoardId() == F("test-board"), "board ID persisted");
	require(configuration.boardProfile.getName() == F("Test board"), "board name persisted");
	std::vector<uint8_t> pins;
	const auto* error = PwmBoardProfile::load(configuration, pins);
	require(error == nullptr, error ? error : "");
	require(pins == std::vector<uint8_t>({5, 0}), "typed profile retains board PWM order");
	require(configuration.boardProfile.reservedGpios[0] == 9, "reservations persisted");
	require(configuration.lights.getItemCount() == 1, "light assignment persisted");
	require(configuration.lights[0].pins[0] == 5, "light GPIO persisted");
}
}

void init()
{
	Serial.begin(115200);
	fileSetFileSystem(&IFS::Host::getFileSystem());
	{
		AppConfig database("board-data");
		{
			auto hardware = AppConfig::Hardware(database).update();
			require(bool(hardware), "hardware updater available");
			MemoryDataStream stream;
			stream << F(R"({"configuration":{
				"board_id":"test-board","board_revision":1,"soc":"esp32c3",
				"board_profile":{"id":"test-board","revision":1,"name":"Test board","soc":"esp32c3",
					"reserved_gpios":[9],"implemented_pins":[
						{"id":"a","label":"A","gpio":5,"allowed_functions":["light.pwm"]},
						{"id":"bus","label":"Bus","gpio":6,"allowed_functions":["i2c.sda"]},
						{"id":"b","label":"B","gpio":0,"allowed_functions":["light.pwm"]}
					]},
				"lights":[{"id":"light","index":0,"name":"Light","type":"SINGLE","pins":[5],"is_default":true}]
			}})");
			auto status = hardware.importFromStream(ConfigDB::Json::format, stream);
			require(bool(status), status.toString().c_str());
		}
		check(database);
	}
	{
		AppConfig database("board-data");
		check(database);
		MemoryDataStream stream;
		AppConfig::Hardware(database).exportToStream(ConfigDB::Json::format, stream);
		AppConfig copy("board-copy");
		{
			auto hardware = AppConfig::Hardware(copy).update();
			auto status = hardware.importFromStream(ConfigDB::Json::format, stream);
			require(bool(status), status.toString().c_str());
		}
		check(copy);
	}
	Serial << "PASS: selected board profile persists and reloads through ConfigDB" << endl;
	std::exit(0);
}
