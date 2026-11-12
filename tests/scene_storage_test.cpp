#include <SmingCore.h>
#include <ConfigDB/Json/Format.h>
#include <Data/Stream/MemoryDataStream.h>
#include <app-data.h>
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

template <typename Object> void import(Object& object, const String& json)
{
	MemoryDataStream stream;
	stream << json;
	auto status = object.importFromStream(ConfigDB::Json::format, stream);
	require(bool(status), status.toString().c_str());
}

void check(AppData& database, int index)
{
	AppData::Scenes scenes(database);
	require(scenes.getItemCount() == 1, "one scene survives persistence");
	auto scene = scenes[0];
	require(scene.settings.getItemCount() == 3, "all light steps survive persistence");
	auto legacy = scene.settings[0];
	require(legacy.getLight() == -1, "legacy target defaults to -1");
	require(legacy.getLightId().length() == 0, "legacy target has no UUID");
	require(legacy.color.asHsv().getCt() == 2700, "HSV temperature is unchanged");
	auto explicitLight = scene.settings[1];
	require(explicitLight.getControllerId() == F("remote-controller"), "remote controller is retained");
	require(explicitLight.getLight() == index, "explicit light index is retained");
	require(explicitLight.getLightId() == F("remote-light"), "explicit light UUID is retained");
	require(explicitLight.color.asRaw().getWw() == 4, "warm white is unchanged");
	require(explicitLight.color.asRaw().getCw() == 5, "cold white is unchanged");
	require(scene.settings[2].getLight() == 8, "another light has its own selector");
	require(scene.settings[2].color.asPreset().getId() == F("remote-preset"), "preset reference is unchanged");
}
} // namespace

void init()
{
	Serial.begin(115200);
	Serial.systemDebugOutput(true);
	fileSetFileSystem(&IFS::Host::getFileSystem());
	{
		AppData database("scene-data");
		{
			auto scenes = AppData::Scenes(database).update();
			require(bool(scenes), "scene store can be updated");
			import(scenes, F(R"([{
				"id": "scene-test", "name": "Test",
				"settings": [
					{"controller_id":"legacy-controller","pos":0,
					 "color":{"hsv":{"h":30,"s":80,"v":60,"ct":2700}}},
					{"controller_id":"remote-controller","light":7,"light_id":"remote-light","pos":0,
					 "color":{"raw":{"r":1,"g":2,"b":3,"ww":4,"cw":5}}},
					{"controller_id":"remote-controller","light":8,"light_id":"other-light","pos":1,
					 "color":{"Preset":{"id":"remote-preset"}}}
				]
			}])"));
		}
		check(database, 7);
		{
			auto scenes = AppData::Scenes(database).update();
			require(bool(scenes), "scene store can be updated again");
			auto scene = scenes[0];
			auto setting = scene.settings[1];
			setting.setLight(12);
		}
		check(database, 12);
	}
	String exported;
	{
		AppData reloaded("scene-data");
		check(reloaded, 12);
		MemoryDataStream stream;
		AppData::Scenes scenes(reloaded);
		scenes.exportToStream(ConfigDB::Json::format, stream);
		stream.moveString(exported);
	}
	{
		AppData copied("scene-copy");
		{
			auto update = AppData::Scenes(copied).update();
			require(bool(update), "copy store can be updated");
			import(update, exported);
		}
		check(copied, 12);
	}
	Serial << "PASS: scene targets persist, reload and export/import unchanged" << endl;
	std::exit(0);
}
