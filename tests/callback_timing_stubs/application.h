#pragma once

#include <string>

struct TestSyslogStream {
	std::string output;
	void printf(const char* format, ...);
};

struct TestApplication {
	TestSyslogStream udpSyslogStream;
};

extern TestApplication app;
