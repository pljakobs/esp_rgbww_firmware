/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 * 			Peter Jakobs http://github.com/pljakobs
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of<
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * https://www.gnu.org/copyleft/gpl.html
 *
 * @section DESCRIPTION
 *
 *
 */


#include <RGBWWCtrl.h>
#include <Ota/Upgrader.h>
#include <RGBWWLed/RGBWWLed.h>
#include <SmingCore.h>
#include <Storage/SysMem.h>
#include <Storage/ProgMem.h>
#include <Storage/Debug.h>
#include <JSON/StreamingParser.h>
#include <VersionListener.h>
#include <FlashString/Stream.hpp>
#include <fileMap.h>
#include <apihandler.h>

#ifdef RSYSLOG
#ifndef SMING_RELEASE
#include <MultiOutputStream.h>
#include <udpSyslogStream.h>
#endif
#endif

#if ARCH_ESP8266
#define PART0 "lfs0"
#elif ARCH_ESP32
#define PART0 "factory"
#endif

//IMPORT_FSTR_LOCAL(default_config, PROJECT_DIR "/default_config.json");

#ifdef ARCH_ESP8266
#include <Platform/OsMessageInterceptor.h>

static OsMessageInterceptor osMessageInterceptor;

/**
 * @brief See if the OS debug message is something we're interested in.
 * @param msg
 * @retval bool true if we want to report this
 */
static bool __noinline parseOsMessage(OsMessage& msg)
{
	m_printf(_F("[OS] %s\r\n"), msg.getBuffer());
	if(msg.startsWith(_F("E:M "))) {
		Serial.println(_F("** OS Memory Error **"));
		return true;
	}
	if(msg.contains(_F(" assert "))) {
		Serial.println(_F("** OS Assert **"));
		return true;
	}
	if(msg.contains(_F("vPortFree"))) {
		Serial.println(_F("** vPortFree **"));
		return true;
	}
	return false;
}

/**
 * @brief Called when the OS outputs a debug message using os_printf, etc.
 * @param msg The message
 */
static void onOsMessage(OsMessage& msg)
{
	// Note: We do the check in a separate function to avoid messing up the stack pointer
	if(parseOsMessage(msg)) {
		if(gdb_present() == eGDB_Attached) {
			gdb_do_break();
		} else {
			//#ifdef ARCH_ESP8266
			register uint32_t sp __asm__("a1");
			debug_print_stack(sp + 0x10, 0x3fffffb0);
			//#endif
		}
	}
}
#endif

#ifdef ARCH_ESP8266

// include partition file  and rboot for initial OTA
namespace
{
// Note: This file won't exist on initial build!
#if defined(SMING_RELEASE)
// release build, pull release partition table
IMPORT_FSTR(partitionTableData, PROJECT_DIR "/out/Esp8266/release/firmware/partitions.bin")
#else
IMPORT_FSTR(partitionTableData, PROJECT_DIR "/out/Esp8266/debug/firmware/partitions.bin")
#endif
} // namespace

extern "C" void __wrap_user_pre_init(void)
{
	static_assert(PARTITION_TABLE_OFFSET == 0x3fa000, "Bad PTO");
	Storage::initialize();
	auto& flash = *Storage::spiFlash;
	if(!flash.partitions()) {
		{
			LOAD_FSTR(data, partitionTableData)
			flash.erase_range(PARTITION_TABLE_OFFSET, flash.getBlockSize());
			flash.write(PARTITION_TABLE_OFFSET, data, partitionTableData.size());
			flash.loadPartitions(PARTITION_TABLE_OFFSET);
		}
	}

	extern void __real_user_pre_init(void);
	__real_user_pre_init();
}

// ─── Crash-dump capture via RTC user memory ──────────────────────────────────
// RTC user area: slots 64-127, each 4 bytes → 256 bytes total.
// Layout: magic(4) + reason(4) + exccause(4) + epc1(4) + epc2(4) + epc3(4)
//         + excvaddr(4) + depc(4) + stackBase(4) + stackCount(4)
//         + stackWords[53](212) = 256 bytes exactly
// Output format is compatible with Sming decode-stacktrace.py.
#define CRASH_RTC_SLOT    64
#define CRASH_RTC_MAGIC   0xDEADC0DEu
#define CRASH_STACK_WORDS 54

struct CrashDump {
	uint32_t magic;
	uint32_t reason;
	uint32_t exccause;
	uint32_t epc1, epc2, epc3;
	uint32_t excvaddr, depc;
	uint32_t stackBase;   // sp at crash time — needed for decode-stacktrace address column
	uint32_t stackCount;
	uint32_t stackWords[CRASH_STACK_WORDS];
};
static_assert(sizeof(CrashDump) == 256, "CrashDump must fit exactly in RTC user memory");

static CrashDump g_crashDump;
static bool g_crashDumpValid = false;

// Overrides the weak alias in crash_handler.c — runs before the reset.
// Keep it minimal: only SDK primitive writes are safe here.
extern "C" void custom_crash_callback(struct rst_info* ri, uint32_t stack, uint32_t stack_end)
{
	CrashDump dump{};
	dump.magic    = CRASH_RTC_MAGIC;
	dump.reason   = ri->reason;
	dump.exccause = ri->exccause;
	dump.epc1     = ri->epc1;
	dump.epc2     = ri->epc2;
	dump.epc3     = ri->epc3;
	dump.excvaddr = ri->excvaddr;
	dump.depc     = ri->depc;
	dump.stackBase = stack;

	uint32_t count = 0;
	for(uint32_t addr = stack; addr < stack_end && count < CRASH_STACK_WORDS; addr += 4) {
		dump.stackWords[count++] = *reinterpret_cast<const uint32_t*>(addr);
	}
	dump.stackCount = count;

	system_rtc_mem_write(CRASH_RTC_SLOT, &dump, sizeof(dump));
}

#endif // ARCH_ESP8266

Application app;

#if !(defined SMING_RELEASE) && (defined RSYSLOG)
MultiOutputStream debugStream;

size_t debugStreamOutputCallback(const char* buffer, unsigned int length)
{
	return debugStream.write((const uint8_t*)buffer, length);
}
#endif

void onReady()
{
	//System.setCpuFrequencye(CF_160MHz);
	app.rtc_info = system_get_rst_info();
	
#ifdef ARCH_ESP8266
	app.readCrashDump();
	osMessageInterceptor.begin(onOsMessage);
	debug_i(ANSI_COLOR_BLUE "starting os message interceptor" ANSI_COLOR_RESET);
#endif

#ifdef ARCH_ESP32
	esp_wifi_set_ps (WIFI_PS_NONE);
#endif
#if !(defined SMING_RELEASE) && (defined RSYSLOG)
	Serial.systemDebugOutput(false); // disable direct Serial hook; output now goes through debugStreamOutputCallback only
	auto oldCallback = m_setPuts(&debugStreamOutputCallback);
	debugStream.addStream(&Serial, false);
	debugStream.addStream(&app.udpSyslogStream, false);
#endif

	// seperated application init
	app.init();

	// Run Services on system ready
	//System.onReady(SystemReadyDelegate(&Application::startServices, &app));
	app.startServices();
}

// Sming Framework INIT method - called during boot
void init(){	

	// use the JTAG-USB serial for debug output if available
	#ifdef UART_ID_SERIAL_USB_JTAG 
        //Serial.setPort(UART_ID_SERIAL_USB_JTAG);
    #endif

    Serial.begin(SERIAL_BAUD_RATE);
    
	Serial.systemDebugOutput(true);
    
	delay(500);
	Serial.setTxBufferSize(512);
	Serial.setTxWait(false); // Make sure debug output doesn't stall
	Serial.systemDebugOutput(true);
	
	// System.setCpuFrequency(CpuCycleClockFast::cpuFrequency());
	debug_i(ANSI_COLOR_BLUE "Available heap: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "\r\n" ANSI_COLOR_RESET, app.getFreeHeapSize());
	debug_i(ANSI_COLOR_BLUE "===starting cpu profiling===" ANSI_COLOR_RESET);
	onReady(); // this is just in preparation for cpu profiling
}

int32_t getVersion(IDataSourceStream& input)
{
	JSON::VersionListener listener;
	JSON::StaticStreamingParser<128> parser(&listener);
	auto status = parser.parse(input);
	if (listener.hasVersion())
	{
		input.seekFrom(0, SeekOrigin::Start); // rewind to leave the stream in the same state
		return listener.getVersion();
	}
	return -1;
}

Application::~Application()
{
	if(pNtpclient != nullptr) {
		delete pNtpclient;
		pNtpclient = nullptr;
	}
}

void Application::uptimeCounter()
{
	++_uptimeMinutes;
	if (_uptimeMinutes % 10 ==0){
		_minimumHeap10min=system_get_free_heap_size();
		_HeapLowErr10min=0;
	}
	
}

void Application::checkRam()
{
	// Create JSON object with uptime and free heap
	StaticJsonDocument<256> doc;
	time_t now = time(nullptr); // should be unix time if ntp is running
	doc[F("id")] = (uint32_t)system_get_chip_id();
	doc[F("time")] = now;	
	doc[F("uptime")] = _uptimeMinutes*60;
	doc[F("ip")] = WifiStation.getIP().toString();
	doc[F("freeHeap")] = getFreeHeapSize();
	doc[F("minHeapRuntime")]=_minimumHeapUptime;
	doc[F("minHeap10min")]=_minimumHeap10min;
	doc[F("heapLowErrUptime")]=_HeapLowErrUptime;
	doc[F("heapLowErr10min")]=_HeapLowErr10min;
	doc[F("firmware")] = fw_git_version;
	doc[F("build")] = BUILD_TYPE;
	doc[F("soc")] = SOC;
	doc[F("neighbours")]=app.controllers->getVisibleCount();
	if (app.rtc_info->reason!= 0 && !_reboot_reported)
	{
		AppConfig::Network::Telemetry telemetryCfg(*cfg);

		doc[F("reboot")][F("number")] = telemetryCfg.getNumReboots();
		doc[F("reboot")][F("reason")] = app.rtc_info->reason;
		doc[F("reboot")][F("exccause")] = app.rtc_info->exccause;
		doc[F("reboot")][F("epc1")] = app.rtc_info->epc1;
		doc[F("reboot")][F("epc2")] = app.rtc_info->epc2;
		doc[F("reboot")][F("epc3")] = app.rtc_info->epc3;
		doc[F("reboot")][F("excvaddr")] = app.rtc_info->excvaddr;
		doc[F("reboot")][F("depc")] = app.rtc_info->depc;
	}
		doc[F("mDNS")][F("received")] = _mDNS_received;
		doc[F("mDNS")][F("replies")] = _mDNS_replies;

	debug_i(ANSI_COLOR_BLUE "Free heap: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ", uptime: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, getFreeHeapSize(), millis() / 1000);
	if (!telemetryClient.stat(doc))
	{
		debug_i(ANSI_COLOR_BLUE "Failed to publish monitor data to telemetry MQTT" ANSI_COLOR_RESET);
		/* 
		if (!telemetryClient.isRunning()){
			debug_i(ANSI_COLOR_BLUE "restarting telemetry MQTT client" ANSI_COLOR_RESET);
			telemetryClient.reconnect();
		}
		*/
	}
	
	if (app.rtc_info->reason!= 0 && !_reboot_reported){
		_reboot_reported=true;	
		AppConfig::Network::Telemetry telemetryCfg(*cfg);
		auto reboots=telemetryCfg.getNumReboots();
		if(auto telemetryUpdate = telemetryCfg.update()){
			telemetryUpdate.setNumReboots(reboots+1);
		}
	} 
}

size_t Application::getFreeHeapSize(){
	size_t fh = system_get_free_heap_size();
	if (fh<_minimumHeapUptime ) _minimumHeapUptime=fh;
	if (fh<_minimumHeap10min) _minimumHeap10min=fh;
	
	return fh;
}

bool Application::checkHeap( uint minHeap)
{
	uint fh = getFreeHeapSize();
	if (fh<6000)
	{
		// minimize heap usage by increasing the minHeap threshold when we're critical anyway. This should preserve some heap for receive packet buffers and thus improve stability
		minHeap=minHeap*1.5;
	}
	if(fh<minHeap){
		_HeapLowErrUptime++;
		_HeapLowErr10min++;
		return false;
	}
	return true;
}

void Application::init()
{
	debug_i(ANSI_COLOR_BLUE "ESP RGBWW Controller Version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\r\n" ANSI_COLOR_RESET, fw_git_version);
	debug_i(ANSI_COLOR_BLUE "Sming Version: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\r\n" ANSI_COLOR_RESET, sming_git_version);

	debug_i(ANSI_COLOR_BLUE "Platform: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\r\n" ANSI_COLOR_RESET, SOC);

#if defined(ARCH_ESP8266) //|| defined(ESP32)
	/*
    * verify for new partition layout
    */
	debug_i(ANSI_COLOR_BLUE "application init, \nspiffs0 found: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\nspiffs1 found: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\nlfs1 found: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\nlfs1 found: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " " ANSI_COLOR_RESET,
			Storage::findPartition(F("spiffs0")) ? F("true") : F("false"),
			Storage::findPartition(F("spiffs1")) ? F("true") : F("false"),
			Storage::findPartition(PART0) ? F("true") : F("false"),
			Storage::findPartition(F("lfs1")) ? F("true") : F("false"));
	if(!Storage::findPartition(PART0) && !Storage::findPartition(F("lfs1"))) {
		// mount existing data partition
		debug_i(ANSI_COLOR_BLUE "application init (with spiffs) => Mounting file system" ANSI_COLOR_RESET);
	
		debug_i(ANSI_COLOR_BLUE "application init => switching file systems - partition 1" ANSI_COLOR_RESET);
		ota.switchPartitions();
		debug_i(ANSI_COLOR_BLUE "application init => saving config" ANSI_COLOR_RESET);
	}

#endif

	//load settings
	_uptimetimer.initializeMs(60000, TimerDelegate(&Application::uptimeCounter, this)).start();
	_checkRamTimer.initializeMs(30000, TimerDelegate(&Application::checkRam, this)).start();

#ifdef ARCH_ESP8266
	// load boot information
	uint8 bootmode, bootslot;
	debug_i(ANSI_COLOR_BLUE "Application::init - loading boot info" ANSI_COLOR_RESET);
	if(rboot_get_last_boot_mode(&bootmode)) {
		if(bootmode == MODE_TEMP_ROM) {
			debug_i(ANSI_COLOR_BLUE "Application::init - temp boot, rebooting after OTA" ANSI_COLOR_RESET);
			System.restart();
		} else {
			debug_i(ANSI_COLOR_BLUE "Application::init - normal boot" ANSI_COLOR_RESET);
		}
		_bootmode = bootmode;
	}
#endif

debug_i(ANSI_COLOR_BLUE "Application::init - check running partition" ANSI_COLOR_RESET);
auto part=app.ota.ota.getRunningPartition();
debug_i(ANSI_COLOR_BLUE "Application::init - running partition " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, part.name());

#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
	mountfs(getRomSlot());
	// ToDo - rework mounting filesystem
	if(_fs_mounted) {
		Directory dir;
		if(dir.open()) {
			while(dir.next()) {
				Serial.print(_F("  "));
				Serial.println(dir.stat().name);
			}
		}
		Serial << dir.count() << _F(" files found") << endl << endl;
	}

//#if defined(ARCH_ESP8266) || defined(ESP32)
	app.ota.checkAtBoot();
//#endif
#endif
	(void)getFreeHeapSize(); // sample heap after fs mount + OTA check
#ifdef ARCH_HOST
	debug_i(ANSI_COLOR_BLUE "mounting host file system" ANSI_COLOR_RESET);
	fileSetFileSystem(&IFS::Host::getFileSystem());
#endif

	// initialize config and data
	cfg =  std::make_unique<AppConfig>(configDB_PATH);
	data = std::make_unique<AppData>(dataDB_PATH);
	api = std::make_unique<Api>();
	controllers = std::make_unique<Controllers>();
	(void)getFreeHeapSize(); // sample heap after ConfigDB + Controllers construction

	// verify if there is a new version of the hardware config
	

	{
		// check if the pinconfig json provided with the frontend is newer than the pinconfig array in the configdb
		// this allows to simply add new pin configs to running controllers
		AppConfig::Hardware hardware(*cfg);
		uint32_t currentVersion=hardware.getVersion();

		debug_i(ANSI_COLOR_BLUE "make pinconfig stream" ANSI_COLOR_RESET);
		FSTR::Stream fs(fileMap["config/pinconfig.json"]);	
		//Serial.println(fileMap["config/pinconfig.json"]);
		debug_i(ANSI_COLOR_BLUE "get file Version" ANSI_COLOR_RESET);
		uint32_t fileVersion=getVersion(fs);
		debug_i(ANSI_COLOR_BLUE "fileVersion " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, fileVersion);	
		if(fileVersion == -1){
			debug_i(ANSI_COLOR_BLUE "Application::init - no version found in pinconfig" ANSI_COLOR_RESET);
		}
		debug_i(ANSI_COLOR_BLUE "Application::init - hardware version: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ", file version: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, currentVersion, fileVersion);
		if(fileVersion>currentVersion){
			if(auto hardwareUpdate = hardware.update()){
				hardwareUpdate.importFromStream(ConfigDB::Json::format, fs);
			}
		}
	}
	{
    	int clearPin = -1;
		{
			AppConfig::General general(*cfg);
			AppConfig::Hardware hardware(*cfg);

			String pinConfigName = general.getCurrentPinConfigName();
			String SoC = SOC;

			if (hardware.pinconfigs.getItemCount() > 0) {
				for (auto pinconfig : hardware.pinconfigs) {
					if (pinconfig.getName() == pinConfigName && pinconfig.getSoc() == SoC) {
						clearPin = pinconfig.getClearPin();
						break;
					}
				}
			}
		}
		debug_i(ANSI_COLOR_BLUE "Application::init - clear pin " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, clearPin);
		if(clearPin >= 0) {
			pinMode(clearPin, INPUT);
			debug_i(ANSI_COLOR_BLUE "Application::init - clear pin set to input" ANSI_COLOR_RESET);
		
			_clearPin = clearPin;
            _resetPinTimer.initializeMs(100, TimerDelegate(&Application::pollResetButton, this)).start();
        }
		#if !defined(ARCH_HOST)

			if(clearPin >=0 && digitalRead(clearPin) < 1) {
				debug_i(ANSI_COLOR_BLUE "CLR button low - resetting settings" ANSI_COLOR_RESET);
				// ConfigDB - decide if to reload defaults or load a specific saved version
				// perhaps by holding the clear pin low for a certain time along with blink codes?
				// cfg.reset();
				network.forgetWifi();
			}

		#endif
	}

	debug_i(ANSI_COLOR_BLUE "Application::init - hardware config loaded" ANSI_COLOR_RESET);
	

// check if we need to reset settings


	// check ota
#ifdef ARCH_ESP8266
	ota.checkAtBoot();
#endif
	
	{
		debug_i(ANSI_COLOR_BLUE "application init => checking ConfigDB" ANSI_COLOR_RESET);
		AppConfig::General general(*cfg);
		debug_i(ANSI_COLOR_BLUE "application init => config is " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, general.getIsInitialized()?"initialized":"not initialized");
		if(!general.getIsInitialized()) {
			debug_i(ANSI_COLOR_BLUE "application init => reading config" ANSI_COLOR_RESET);

			debug_i(ANSI_COLOR_BLUE "Application::init - first run" ANSI_COLOR_RESET);
			_first_run = true;

			if(auto generalUpdate = general.update()) {
				generalUpdate.setIsInitialized(true);
			}

		} else {
			debug_i(ANSI_COLOR_BLUE "ConfigDB already initialized. resetting hardware definition" ANSI_COLOR_RESET);
			{
			if (auto generalUpdate= general.update()){
				generalUpdate.supportedColorModels.loadArrayDefaults();
				}
			}
			{
				AppConfig::Hardware hardware(*app.cfg);
				if(auto hardwareUpdate=hardware.update()){
					hardwareUpdate.availablePins.loadArrayDefaults();
				}
			}
		}
	}

	// prepare active hosts list
	{
		// pre-allocate heap for the visible hosts vector. 
		// this should adapt to the number of hosts detected in earlier boots
		AppData::Root::Controllers controllers(*app.data);
		
		auto myId=(uint32_t)system_get_chip_id();
		AppConfig::General general(*cfg);
		String myName=general.getDeviceName();
		if(myName.length()<=0){
			char myName_buf[64];
			snprintf(myName_buf, sizeof(myName_buf), "rgbww-%x", myId);
			myName = myName_buf;
		}
		app.controllers->addOrUpdate( myId,myName, WifiStation.getIP().toString(), 1200); // add myself to the list
	}

	/*
	Serial << endl << _F("** Stream **") << endl;
	Serial << "#########################################################################################"<<endl;
	cfg->exportToStream(ConfigDB::Json::format, Serial);
	Serial <<endl;
	Serial << "#########################################################################################"<<endl;
	*/

	
	/// initialize led ctrl
	rgbwwctrl.init();
	debug_i(ANSI_COLOR_BLUE "ledctrl initialized" ANSI_COLOR_RESET);
	(void)getFreeHeapSize(); // sample heap after LED ctrl init (PWM + color config)

	initButtons();
	debug_i(ANSI_COLOR_BLUE "buttons initialized" ANSI_COLOR_RESET);

	// initialize webserver
	app.webserver.init();
	debug_i(ANSI_COLOR_BLUE "webserver initialized" ANSI_COLOR_RESET);
	(void)getFreeHeapSize(); // sample heap after route registration

	debug_i(ANSI_COLOR_BLUE "pin config string " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, fileMap["pin_config"]);

	debug_i(ANSI_COLOR_BLUE "start network init" ANSI_COLOR_RESET);
	// initialize networking
	network.init();
	debug_i(ANSI_COLOR_BLUE "network initizalized, ssid: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, WifiStation.getSSID().c_str());
	(void)getFreeHeapSize(); // sample heap after WiFi init
	{
		AppConfig::Network network(*cfg);
		if(network.rsyslog.getEnabled()) {
			String host = network.rsyslog.getHost();
			uint16_t port = network.rsyslog.getPort();
			AppConfig::General general(*cfg);
			String myName=general.getDeviceName();
			debug_i(ANSI_COLOR_BLUE "Initializing remote syslog with host " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " and port " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, host.c_str(), port);
#if !(defined SMING_RELEASE) && (defined RSYSLOG)
			app.udpSyslogStream.begin(host, port, myName, F("Lightinator"));
#endif
		} else {
			debug_i(ANSI_COLOR_BLUE "Remote syslog disabled" ANSI_COLOR_RESET);
		}
	}
	
	
}
void Application::initButtons()
{
	Vector<String> buttons;
	{
		debug_i(ANSI_COLOR_BLUE "Application::initButtons" ANSI_COLOR_RESET);
		AppConfig::General general(*cfg);

		if(general.getButtonsConfig().length() <= 0)
			return;

		String buttonsConfig = general.getButtonsConfig();

		debug_i(ANSI_COLOR_BLUE "Configuring buttons using string: '" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "'" ANSI_COLOR_RESET, buttonsConfig.c_str());

		splitString(buttonsConfig, ',', buttons);
	} // end of ConfigDB general context

	for(uint32_t i = 0; i < buttons.count(); ++i) {
		if(buttons[i].length() == 0)
			continue;

		uint32_t pin = buttons[i].toInt();
		if(pin >= _lastToggles.size()) {
			debug_i(ANSI_COLOR_BLUE "Pin " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE " is invalid. Max is " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, pin, _lastToggles.size() - 1);
			continue;
		}
		debug_i(ANSI_COLOR_BLUE "Configuring button: '" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "'" ANSI_COLOR_RESET, buttons[i].c_str());

		_lastToggles[pin] = 0ul;

		attachInterrupt(pin, std::bind(&Application::onButtonTogglePressed, this, pin), FALLING);
		pinMode(pin, INPUT_PULLUP);
	}
}

// Will be called when system initialization was completed
void Application::startServices()
{
	debug_i(ANSI_COLOR_BLUE "Application::startServices" ANSI_COLOR_RESET);

	rgbwwctrl.start();
	webserver.start();

	{
		debug_i(ANSI_COLOR_BLUE "Application::startServices - starting NTP" ANSI_COLOR_RESET);
		AppConfig::Root appcfg(*cfg);
		if(appcfg.events.getServerEnabled()) {
			eventserver.setEnabled(true);
			eventserver.start(app.webserver);
		}
	} // end of ConfigDB root context
}
void Application::startNetworkServices()
{
	debug_i(ANSI_COLOR_BLUE "Application::startServices - starting mqtt" ANSI_COLOR_RESET);
	bool mqttEnabled = false;
	{
		AppConfig::Network network(*cfg);
		AppConfig::General general(*cfg);
		debug_i(ANSI_COLOR_BLUE "Application::startServices - mqtt enabled: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, network.mqtt.getEnabled() ? "true" : "false");
		String mqttClientId = network.mqtt.homeassistant.getNodeId();
		if(mqttClientId.length() > 0) {
			debug_i(ANSI_COLOR_BLUE "Application::startServices - mqtt client id: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, mqttClientId.c_str());
		} else {
			if(general.getDeviceName().length() > 0) {
				mqttClientId = general.getDeviceName();
			} else {
				mqttClientId = F("rgbww_") + String(WifiStation.getMAC());
			}
		}
		mqttEnabled = network.mqtt.getEnabled();
	} // close ConfigDB contexts before mqttclient.init() opens its own
	mqttclient.init(); // initialize mqtt client with node name
	debug_i(ANSI_COLOR_BLUE "Application::startServices - mqtt client initialized" ANSI_COLOR_RESET);
	(void)getFreeHeapSize(); // sample heap after MQTT client init
	if(mqttEnabled) {
		mqttclient.start();
	}
	telemetryClient.start();
	
}

void Application::stopServices()
{
	debug_i(ANSI_COLOR_BLUE "Application::stopServices" ANSI_COLOR_RESET);

	// Stop timers first so no new work is queued while sockets are closing.
	_systimer.stop();
	_uptimetimer.stop();
	_checkRamTimer.stop();
	_resetPinTimer.stop();

	rgbwwctrl.stop();
	eventserver.stop();
	webserver.stop();
	mqttclient.stop();
	telemetryClient.stop();

	if(network.isApActive()) {
		network.stopAp();
	}
}

void Application::logRestart(){
	char msg[128];
	m_snprintf(msg, sizeof(msg), "restart, reason: %u, exccause: %u", 
	           (unsigned int)app.rtc_info->reason, (unsigned int)app.rtc_info->exccause);
	telemetryClient.log(msg);
}

#ifdef ARCH_ESP8266
void Application::readCrashDump()
{
	CrashDump dump{};
	system_rtc_mem_read(CRASH_RTC_SLOT, &dump, sizeof(dump));
	if(dump.magic == CRASH_RTC_MAGIC) {
		g_crashDump = dump;
		g_crashDumpValid = true;
		// clear magic so we don't re-report on the next boot
		dump.magic = 0;
		system_rtc_mem_write(CRASH_RTC_SLOT, &dump, sizeof(dump));
	}
}
#endif

void Application::reportCrashDump()
{
	bool fullDumpReported = false;
#ifdef ARCH_ESP8266
	if(g_crashDumpValid) {
		g_crashDumpValid = false;
		fullDumpReported = true;
		// Emit in the format that Sming decode-stacktrace.py recognises.
		// "pc=" line puts the tool into IN_REGISTERS state.
		debug_w(ANSI_COLOR_YELLOW "pc=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " sp=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " excvaddr=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW "" ANSI_COLOR_RESET,
		        g_crashDump.epc1, g_crashDump.stackBase, g_crashDump.excvaddr);
		// Emit remaining exception registers on a separate line
		// (the tool picks these up as generic r00/r01 style or passes them through)
		debug_w(ANSI_COLOR_YELLOW "epc2=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " epc3=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " exccause=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW " depc=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " reason=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW "" ANSI_COLOR_RESET,
		        g_crashDump.epc2, g_crashDump.epc3,
		        g_crashDump.exccause, g_crashDump.depc, g_crashDump.reason);
		// Stack dump in the format "xxxxxxxx:  XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX"
		debug_w(ANSI_COLOR_YELLOW "Stack dump:" ANSI_COLOR_RESET);
		uint32_t addr = g_crashDump.stackBase;
		for(uint32_t i = 0; i < g_crashDump.stackCount; i += 4, addr += 16) {
			uint32_t w0 = g_crashDump.stackWords[i];
			uint32_t w1 = (i + 1 < g_crashDump.stackCount) ? g_crashDump.stackWords[i + 1] : 0;
			uint32_t w2 = (i + 2 < g_crashDump.stackCount) ? g_crashDump.stackWords[i + 2] : 0;
			uint32_t w3 = (i + 3 < g_crashDump.stackCount) ? g_crashDump.stackWords[i + 3] : 0;
			debug_w(ANSI_COLOR_YELLOW "" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW ":  " ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " " ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " " ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " " ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW "" ANSI_COLOR_RESET, addr, w0, w1, w2, w3);
		}
	}
#endif
	// Fallback: emit reason/registers if we didn't already emit a full dump above.
	// On ESP32 this is always the path (no crash callback available).
	// On ESP8266 this fires only if the RTC magic was invalid (rare: RTC scrambled on hard reset).
	if(!fullDumpReported && rtc_info != nullptr &&
	   (rtc_info->reason == REASON_EXCEPTION_RST ||
	    rtc_info->reason == REASON_SOFT_WDT_RST  ||
	    rtc_info->reason == REASON_WDT_RST)) {
		debug_w(ANSI_COLOR_YELLOW "*** CRASH REBOOT: reason=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW " exccause=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW " epc1=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW " excvaddr=0x" ANSI_COLOR_CYAN "%08x" ANSI_COLOR_YELLOW "" ANSI_COLOR_RESET,
		        rtc_info->reason, rtc_info->exccause, rtc_info->epc1, rtc_info->excvaddr);
	}
}
void Application::restart()
{
	static bool gracefulRestartInProgress = false;

	debug_i(ANSI_COLOR_BLUE "Application::restart" ANSI_COLOR_RESET);
	if(network.isApActive()) {
		network.stopAp();
		_systimer.initializeMs(500, TimerDelegate(&Application::restart, this)).startOnce();
		return;
	}

	if(!gracefulRestartInProgress) {
		gracefulRestartInProgress = true;
		stopServices();
		// Give network stacks a short window to process close callbacks cleanly.
		_systimer.initializeMs(250, TimerDelegate(&Application::restart, this)).startOnce();
		return;
	}

	System.restart();
}

void Application::reset()
{
	debug_i(ANSI_COLOR_BLUE "Application::reset" ANSI_COLOR_RESET);
	//cfg.reset();
	rgbwwctrl.colorReset();
	network.forgetWifi();
	delay(500);
	restart();
}

void Application::forget_wifi_and_restart()
{
	debug_i(ANSI_COLOR_BLUE "Application::forget_wifi_and_restart" ANSI_COLOR_RESET);
	network.forgetWifi();
	_systimer.initializeMs(500, TimerDelegate(&Application::restart, this)).startOnce();
}

bool Application::delayedCMD(String cmd, int delay)
{
	debug_i(ANSI_COLOR_BLUE "Application::delayedCMD cmd: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " - delay: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, cmd.c_str(), delay);
	if(cmd.equals(F("reset"))) {
		wsBroadcast(F("notification"), F("Controller will reset and restart"));
		wsBroadcast(F("webapp_cmd"), F("reload"));
		telemetryClient.log(F("delaycmd reset"));
		_systimer.initializeMs(delay, TimerDelegate(&Application::reset, this)).startOnce();
	} else if(cmd.equals(F("restart"))) {
		wsBroadcast(F("notification"), F("Controller will restart"));
		wsBroadcast(F("webapp_cmd"), F("reload"));
		telemetryClient.log(F("delaycmd restart"));
		_systimer.initializeMs(delay, TimerDelegate(&Application::restart, this)).startOnce();
	} else if(cmd.equals(F("clear_ota_restart"))) {
		debug_i(ANSI_COLOR_BLUE "Application::delayedCMD: clearing OTA status before restart" ANSI_COLOR_RESET);
		ota.saveStatus(OTASTATUS::OTA_NOT_UPDATING);
		wsBroadcast(F("notification"), F("Controller will restart (OTA status cleared)"));
		wsBroadcast(F("webapp_cmd"), F("reload"));
		telemetryClient.log(F("delaycmd clear_ota_restart"));
		_systimer.initializeMs(delay, TimerDelegate(&Application::restart, this)).startOnce();
	} else if(cmd.equals(F("stopap"))) {
		wsBroadcast(F("notification"), F("Controller will disable the access point"));
		network.stopAp(2000);
	} else if(cmd.equals(F("forget_wifi"))) {
		wsBroadcast(F("notification"), F("Controller will reset wifi settings"));
		_systimer.initializeMs(delay, TimerDelegate(&AppWIFI::forgetWifi, &network)).startOnce();
	} else if(cmd.equals(F("forget_wifi_and_restart"))) {
		wsBroadcast(F("notification"), F("Controller will reset wifi settings and restart"));
		network.forgetWifi();
		wsBroadcast(F("webapp_cmd"), F("reload"));
		_systimer.initializeMs(delay, TimerDelegate(&Application::forget_wifi_and_restart, this)).startOnce();
	} else if(cmd.equals(F("umountfs"))) {
		//umountfs();
	} else if(cmd.equals(F("mountfs"))) {
		//
	} else if(cmd.equals(F("switch_rom"))) {
		wsBroadcast(F("notification"), F("Controller will switch to other rom"));
		wsBroadcast(F("webapp_cmd"), F("reload"));
		telemetryClient.log(F("delaycmd switch_rom"));
		_systimer.initializeMs(delay, TimerDelegate(&Application::switchRom, this)).startOnce();
	} else if(cmd.equals(F("forget_controllers"))){
		app.controllers->forgetControllers();
		wsBroadcast(F("notification"), F("Controller list cleared"));
	}else {
		return false;
	}
	return true;
}

void Application::listSpiffsPartitions()
{
	Serial.println(_F("** Enumerate registered partitions"));
	mountfs(1);
	listFiles();
	mountfs(0);
	listFiles();
}

bool Application::mountfs(int slot)
{
	/*
    *
    * need a new mount method for the transitional time when the data file
    * system could be spiffs or LitleFS
    *
    */

#ifdef ARCH_HOST
	/*
     * host file system
     */
	debug_i(ANSI_COLOR_BLUE "mounting host file system" ANSI_COLOR_RESET);
	fileSetFileSystem(&IFS::Host::getFileSystem());
	_fs_mounted = true;
	return _fs_mounted;
#else
	/*
     * on device file system
     */

	auto part = Storage::findPartition(F("spiffs") + String(slot));
	if(part) {
		debug_i(ANSI_COLOR_BLUE "mouting spiffs partition " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE " at " ANSI_COLOR_CYAN "%x" ANSI_COLOR_BLUE ", length " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, slot, part.address(), part.size());
		_fs_mounted = spiffs_mount(part);
		return _fs_mounted;
	} else {
		part = Storage::findPartition(F("lfs0"));
		if(part) {
			debug_i(ANSI_COLOR_BLUE "mouting primary littlefs partition at " ANSI_COLOR_CYAN "%x" ANSI_COLOR_BLUE ", length " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, part.address(), part.size());
			if(lfs_mount(part)) {
				_fs_mounted = true;
				return _fs_mounted;
			} else {
				auto secondary = Storage::findPartition(F("lfs1"));
				if(!secondary) {
					debug_e(ANSI_COLOR_RED "primary partition mount failed and secondary lfs partition was not found" ANSI_COLOR_RESET);
					_fs_mounted = false;
					return _fs_mounted;
				}
				debug_e(ANSI_COLOR_RED "primary partition mount failed, mounting secondary lfs partition  at " ANSI_COLOR_CYAN "%x" ANSI_COLOR_RED ", length " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET,
						secondary.address(), secondary.size());
				_fs_mounted = lfs_mount(secondary);
				return _fs_mounted;
			};
		}
		debug_i(ANSI_COLOR_BLUE "partition is neither spiffs nor lfs" ANSI_COLOR_RESET);
		_fs_mounted = false;
		return _fs_mounted;
	}
#endif
}

void Application::listFiles()
{
	if(FileHandle file = fileOpen(F("VERSION"), IFS::OpenFlag::Read)) {
		debug_i(ANSI_COLOR_BLUE "found VERSION file" ANSI_COLOR_RESET);
		char buffer[64];
		int bytesRead = fileRead(file, buffer, sizeof(buffer));
		if(bytesRead < 0) {
			debug_e(ANSI_COLOR_RED "Failed reading VERSION file" ANSI_COLOR_RESET);
			fileClose(file);
			return;
		}
		if(bytesRead >= static_cast<int>(sizeof(buffer))) {
			bytesRead = sizeof(buffer) - 1;
		}
		buffer[bytesRead] = '\0';
		debug_i(ANSI_COLOR_BLUE "\nweb app version String: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, buffer);
		fileClose(file);
	} else {
		debug_i(ANSI_COLOR_BLUE "Partition has no version file\n" ANSI_COLOR_RESET);
	}

	Directory dir;
	if(dir.open()) {
		while(dir.next()) {
			debug_i(ANSI_COLOR_CYAN " %s" ANSI_COLOR_RESET, dir.stat().name);
		}
	}
	debug_i( ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE " files found" ANSI_COLOR_RESET, dir.count());
}

void Application::umountfs()
{
	/*
    debug_i(ANSI_COLOR_BLUE "Application::umountfs" ANSI_COLOR_RESET);
    auto part = Storage::findPartition(F("spiffs")+String(slot));
    if (part){
        debug_i(ANSI_COLOR_BLUE "unmouting spiffs partition " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE " at " ANSI_COLOR_CYAN "%x" ANSI_COLOR_BLUE ", length " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, slot,part.address(), part.size());
        return spiffs_unmount(part);
    }else{
        part = Storage::findPartition(F("littlefs")+String(slot));
        if(part){
            debug_i(ANSI_COLOR_BLUE "mouting littlefs partition " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE " at " ANSI_COLOR_CYAN "%x" ANSI_COLOR_BLUE ", length " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, slot,part.address(), part.size());
            return true;
            //lfs doesn't seem to define umount
            //return lfs_umount(part);
        }
        debug_i(ANSI_COLOR_BLUE "partition is neither spiffs nor lfs" ANSI_COLOR_RESET);
    }
    */
}
#if defined(ARCH_ESP8266) || defined(ESP32) || defined(ARCH_HOST)
void Application::switchRom()
{
	//ToDo - rewrite to use ota.getRunningPartition() and ota.getNextBootPartition()
	debug_i(ANSI_COLOR_BLUE "Application::switchRom" ANSI_COLOR_RESET);
	app.ota.doSwitch();
}
#else
void Application::switchRom(){}
#endif

int Application::getRomSlot()
{
	auto partition = app.ota.getRomPartition();
	uint8_t slot;
	if(partition.name() == F("rom1")) {
		slot = 1;
	} else {
		slot = 0;
	}
	return slot;
}

/*
*	send a jsonrpc message from a fully constructed JsonRpcMessage object string
*/
void Application::wsBroadcast(String message)
{
    size_t length = message.length();
    if(length > MAX_LOG_LINE_SIZE) length = MAX_LOG_LINE_SIZE;

    char* buffer = new char[length + 1]; // +1 for null terminator
    message.toCharArray(buffer, length + 1);

    app.webserver.wsSendBroadcast(buffer, length);

    delete[] buffer;
}

/*
*	build a jsonrpc message from a command and a parameters string
*/
void Application::wsBroadcast(String cmd, String message)
{
	JsonRpcMessage msg(cmd);
	msg.setId(jsonrpc_id++);
	JsonObject root = msg.getParams();
	root[F("message")] = message;

	String jsonStr = Json::serialize(msg.getRoot());
	wsBroadcast(jsonStr);
}

void Application::wsBroadcast(const String& cmd, const JsonObject& params)
{
	JsonRpcMessage msg(cmd);
	msg.setId(jsonrpc_id++);
	JsonObject root = msg.getParams();
    for (JsonPair kv : params) {
        root[kv.key()] = kv.value();
    }
	String jsonStr = Json::serialize(msg.getRoot());
	//debug_i(ANSI_COLOR_BLUE "Application::wsBroadcast: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, jsonStr.c_str());
	wsBroadcast(jsonStr);
}

void Application::onCommandRelay(const String& method, const JsonObject& params)
{
	debug_i(ANSI_COLOR_BLUE "Application::onCommandRelay" ANSI_COLOR_RESET);
	AppConfig::Sync sync(*cfg);
	if(sync.getCmdMasterEnabled())
		mqttclient.publishCommand(method, params);
}

void Application::onButtonTogglePressed(int pin)
{
	/*
	uint32_t now = millis();
	uint32_t diff = now - _lastToggles[pin];
	debug_i(ANSI_COLOR_BLUE "Application::onButtonTogglePressed" ANSI_COLOR_RESET);
	AppConfig::General general(*cfg);
	if(diff > (uint32_t)general.getButtonsDebounceMs()) { // debounce
		debug_i(ANSI_COLOR_BLUE "Button " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE " pressed - toggle" ANSI_COLOR_RESET, pin);
		rgbwwctrl.toggle();
		_lastToggles[pin] = now;
	} else {
		debug_d("Button press ignored by debounce. Diff: %d Debounce: %d", diff, general.getButtonsDebounceMs());
	}
	*/
}

void Application::pollResetButton()
{
    // Static counter to keep track of hold duration across function calls
    static int holdCounter = 0;

    if (_clearPin < 0) return;

    // Check if button is pressed (Active LOW)
    if (digitalRead(_clearPin) == LOW) {
        holdCounter++;
        
        // 30 ticks * 100ms = 3000ms (3 seconds)
        if (holdCounter >= 30) {
            debug_w(ANSI_COLOR_YELLOW "Emergency ROM Switch triggered via Reset Pin!" ANSI_COLOR_RESET);

            // Reset counter to prevent multiple triggers (though we reboot anyway)
            holdCounter = 0;

            // Visual feedback
            for(int i = 0; i < 4; i++) {
                rgbwwctrl.toggle();
                delay(250);
            }

            switchRom();
            delayedCMD(F("restart"),1000);
        }
    } else {
        // Button released, reset counter
        holdCounter = 0;
    }
}

uint32_t Application::getUptime()
{
	return _uptimeMinutes * 60u;
}
