#pragma once

#if STATUS_DISPLAY
#include <Adafruit_SSD1306.h>
#include <memory>

class StatusDisplay : public Adafruit_SSD1306 {
public:
    static std::unique_ptr<StatusDisplay> createIfConnected(uint8_t sda, uint8_t scl, uint8_t address);
    void refresh(const String& name, const String& address);
    void refreshAccessPoint(const String& address, const String& ssid, const String& password);

private:
    StatusDisplay();
    void drawTextLines(const String& text, unsigned top, unsigned capacity, bool truncate = false);

    String lastName;
    String lastAddress;
    String lastPassword;
    bool rendered = false;
    bool accessPointMode = false;
};
#endif