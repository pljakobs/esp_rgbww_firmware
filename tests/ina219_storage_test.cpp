#include <SmingCore.h>
#include <extension_ina219.h>
#include <cmath>
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
		ExtensionIna219 db("ina219-settings");
		ExtensionIna219::Root root(db);
		require(root.getEnabled() && root.getAddress() == 0x40, "default presence settings");
		require(std::fabs(root.getShuntOhms().asFloat() - 0.1) < 0.000001, "default shunt");
		require(root.getIntervalMs() == 1000, "default interval");
		auto update = root.update();
		require(bool(update), "obtain update");
		update.setAddress(0x41);
		update.setShuntOhms(ConfigDB::Number(0.05));
		update.setIntervalMs(2500);
		require(update.commit(), "persist settings");
	}
	{
		ExtensionIna219 db("ina219-settings");
		ExtensionIna219::Root root(db);
		require(root.getAddress() == 0x41, "address survives reload");
		require(std::fabs(root.getShuntOhms().asFloat() - 0.05) < 0.000001, "shunt survives reload");
		require(root.getIntervalMs() == 2500, "interval survives reload");
	}
	Serial << "PASS: INA219 settings persisted" << endl;
	std::exit(0);
}
