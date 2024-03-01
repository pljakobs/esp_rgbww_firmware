#!/bin/bash
BASE_URL=http://192.168.29.10:8080

WEBROOT=/html
mkdir -p $WEBROOT/Esp8266/v1
mkdir -p $WEBROOT/Esp8266/v2
mkdir -p $WEBROOT/Esp32/v2

# pull and build the webapp
cd /esp_rgb_webapp2
git pull
git checkout devel
git tag nightly $(git describe --abbrev=4 --dirty --always --tags)

WEBAPP_VERSION=$(git describe --abbrev=4 --dirty --always --tags)

npx quasar build
./minifyFontnames.sh
./gzipSPA.sh

echo $WEBAPP_VERSION > dist/spa/VERSION
rf -rf /esp_rgbww_firmware/spiffs/*

cp -a dist/spa/ /esp_rgbww_firmware/spiffs

# pull and build the firmware
cd /esp_rgbww_firmware
git pull
git checkout devel
old_tags=$(git describe --abbrev=4 --dirty --always --tags)
git tag nightly $old_tags

FW_VERSION=$(git describe --abbrev=4 --dirty --always --tags)

make clean
make -j8 SMING_SOC=esp8266 PART_LAYOUT=v1

echo "deploying OTA ESP8266 v1 files to webserver"
cp out/Esp8266/debug/firmware/rom0.bin $WEBROOT/Esp8266/v1
cp out/Esp8266/debug/firmware/spiff_rom.bin $WEBROOT/Esp8266/v1

#  once v2 is implemented
# make -j8 SMING_SOC=esp8266 PART_LAYOUT=v2

# echo "deploying OTA ESP8266 v2 files to webserver"
# cp out/Esp8266/debug/firmware/rom0.bin $WEBROOT/Esp8266/v2
# cp out/Esp8266/debug/firmware/spiff_rom.bin $WEBROOT/Esp8266/v2

# once esp32 is implemented
# make -j8 SMING_SOC=esp32 PART_LAYOUT=v2

# echo "deploying OTA ESP32 v2 files to webserver"
# cp out/Esp8266/debug/firmware/rom0.bin $WEBROOT/Esp32/v2
# cp out/Esp8266/debug/firmware/spiff_rom.bin $WEBROOT/Esp32/v2

# once esp32c3 is implemented
# make -j8 SMING_SOC=esp32c3 PART_LAYOUT=v2

# echo "deploying OTA ESP32c3 v2 files to webserver"
# cp out/Esp8266/debug/firmware/rom0.bin $WEBROOT/Esp32c3/v2
# cp out/Esp8266/debug/firmware/spiff_rom.bin $WEBROOT/Esp32c3/v2

cat <<EOF > $WEBROOT/version.json
{
    "time": "$(date)",
	"rom":
	{
		"fw_version":"$FW_VERSION",
		"url":"$BASE_URL/Esp8266/v1/rom0.bin"
	},
	"spiffs":
	{
		"webapp_version":"$WEBAPP_VERSION",
		"url":"$BASE_URL/Esp8266/v1/spiff_rom.bin"
	},
	"firmware":
	[
		{
			"partitioning":"v1",
			"soc":"Esp8266",
			"files":
			{
				"rom":
				{
					"fw_version":"$FW_VERSION",
					"url":"$BASE_URL/Esp8266/v1/rom0.bin"
				},
				"spiffs":
				{
					"webapp_version":"$WEBAPP_VERSION",
			                "url":"$BASE_URL/Esp8266/v1/spiff_rom.bin"
				}
			}
		},
		{
			"partitioning":"v2",
			"soc":"Esp8266",
			"files":
			{
				"rom":
				{
					"fw_version":"$FW_VERSION",
					"url":"$BASE_URL/Esp8266/v2/rom0.bin"
				},
				"spiffs":
				{
					"webapp_version":"$WEBAPP_VERSION",
			                "url":"$BASE_URL/Esp8266/v2/spiff_rom.bin"
				}
			}
		},
		{
			"partitioning":"v2",
			"soc":"Esp32",
			"files":
			{
				"rom":
				{
					"fw_version":"$FW_VERSION",
					"url":"$BASE_URL/Esp32/v2/rom0.bin"
				},
				"spiffs":
				{
					"webapp_version":"$WEBAPP_VERSION",
			                "url":"$BASE_URL/Esp32/v2/spiff_rom.bin"
				}
			}	
		},
		{
			"partitioning":"v2",
			"soc":"Esp32c3",
			"files":
			{
				"rom":
				{
					"fw_version":"$FW_VERSION",
					"url":"$BASE_URL/Esp32c3/v2/rom0.bin"
				},
				"spiffs":
				{
					"webapp_version":"$WEBAPP_VERSION",
			                "url":"$BASE_URL/Esp32c3/v2/spiff_rom.bin"
				}
			}	
		}
	]
}
EOF
