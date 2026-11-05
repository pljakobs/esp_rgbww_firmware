#if STATUS_DISPLAY
#include <RGBWWCtrl.h>
#include <statusDisplay.h>
#include <Wire.h>

StatusDisplay::StatusDisplay() : Adafruit_SSD1306(128, 64, &Wire, -1)
{
}

std::unique_ptr<StatusDisplay> StatusDisplay::createIfConnected(uint8_t sda, uint8_t scl, uint8_t address)
{
    static_assert(STATUS_DISPLAY_SDA != STATUS_DISPLAY_SCL, "Display SDA and SCL must be different GPIOs");
    static_assert(STATUS_DISPLAY_ADDRESS >= 0x08 && STATUS_DISPLAY_ADDRESS <= 0x77, "Invalid I2C address");
    cdebug_i(STATUSDISPLAY, "StatusDisplay::createIfConnected: probing I2C address 0x%02x, SDA GPIO%u, SCL GPIO%u",
        unsigned(address), unsigned(sda), unsigned(scl));
    Wire.begin(sda, scl);
    Wire.beginTransmission(address);
    const auto probeResult = Wire.endTransmission();
    if(probeResult != TwoWire::I2C_ERR_SUCCESS) {
        if(probeResult == TwoWire::I2C_ERR_ADDR_NACK) {
            cdebug_i(STATUSDISPLAY, "StatusDisplay::createIfConnected: no display at 0x%02x; display not instantiated",
                unsigned(address));
        } else {
            cdebug_w(STATUSDISPLAY, "StatusDisplay::createIfConnected: I2C probe failed at 0x%02x, error %u; display not instantiated",
                unsigned(address), unsigned(probeResult));
        }
        return nullptr;
    }

    std::unique_ptr<StatusDisplay> result(new StatusDisplay);
    if(!result) {
        cdebug_e(STATUSDISPLAY, "StatusDisplay::createIfConnected: failed to allocate display object");
        return nullptr;
    }
    if(!result->begin(SSD1306_SWITCHCAPVCC, address, false, false)) {
        cdebug_e(STATUSDISPLAY, "StatusDisplay::createIfConnected: SSD1306 initialization failed; unable to allocate framebuffer");
        return nullptr;
    }
    result->setTextSize(1);
    result->setTextColor(WHITE);
    result->setTextWrap(false);
    cdebug_i(STATUSDISPLAY, "StatusDisplay::createIfConnected: SSD1306 128x64 initialized at 0x%02x",
        unsigned(address));
    return result;
}

void StatusDisplay::refresh(const String& name, const String& address)
{
    if(rendered && !accessPointMode && name == lastName && address == lastAddress) {
        return;
    }
    if(!rendered || accessPointMode) {
        cdebug_i(STATUSDISPLAY, "StatusDisplay::refresh: showing controller status");
    }
    cdebug_d(STATUSDISPLAY, "StatusDisplay::refresh: name '%s', IP %s", name.c_str(), address.c_str());
    lastName = name;
    lastAddress = address;
    lastPassword = String();
    rendered = true;
    accessPointMode = false;

    clearDisplay();
    setCursor(0, 0);
    print(F("Controller"));
    drawTextLines(name, 12, 63, true);
    setCursor(0, 44);
    print(F("IP address"));
    setCursor(0, 56);
    print(address);
    display();
}

void StatusDisplay::refreshAccessPoint(const String& address, const String& ssid, const String& password)
{
    if(rendered && accessPointMode && address == lastAddress && ssid == lastName && password == lastPassword) {
        return;
    }
    if(!rendered || !accessPointMode) {
        cdebug_i(STATUSDISPLAY, "StatusDisplay::refreshAccessPoint: showing access point status");
    }
    cdebug_d(STATUSDISPLAY, "StatusDisplay::refreshAccessPoint: SSID '%s', IP %s, security %s",
        ssid.c_str(), address.c_str(), password.length() == 0 ? "open" : "secured");
    lastAddress = address;
    lastName = ssid;
    lastPassword = password;
    rendered = true;
    accessPointMode = true;

    clearDisplay();
    setCursor(0, 0);
    print(F("AP ON "));
    print(address);
    drawTextLines(F("SSID: ") + ssid, 8, 42);
    setCursor(0, 24);
    print(F("Password:"));
    drawTextLines(password.length() == 0 ? String(F("(open network)")) : password, 32, 84);
    display();
}

void StatusDisplay::drawTextLines(const String& text, unsigned top, unsigned capacity, bool truncate)
{
    for(unsigned index = 0; index < text.length() && index < capacity; ++index) {
        if(index % 21 == 0) {
            setCursor(0, top + (index / 21) * 8);
        }
        const char character = text[index];
        if(truncate && text.length() > capacity && index >= capacity - 3) {
            write('.');
        } else {
            write(character >= 32 && character <= 126 ? character : '?');
        }
    }
}
#endif