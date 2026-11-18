#pragma once
#include <string>
#include <vector>

enum class DeviceKind { Mouse, Keyboard, Headset, Gamepad, Phone, Other };

enum class Link {
    Unknown,
    Receiver24GHz,
    UsbCable,
    Bluetooth,
    ChargingDock,
    OmniReceiver,
    ReceiverOrBt,
    HeadsetControl,
    XboxWireless,
    PhoneLink
};

enum class Level { None, Empty, Low, Medium, High, Full };

struct DeviceStatus {
    std::wstring key;       // stable per-device id
    std::wstring name;
    std::wstring via;       // "2.4 GHz receiver", "Bluetooth", "USB cable", ...
    std::wstring detail;    // optional breakdown for the tooltip/card ("L 85%, R 80%, case 90%")
    std::wstring hint;      // optional driver-specific informational hint (e.g. COM port busy)
    DeviceKind kind = DeviceKind::Other;
    Link link = Link::Unknown;
    Level level = Level::None;
    int percent = -1;       // 0..100, or -1 if unknown
    std::wstring levelText; // optional coarse label ("Full", "Medium", ...) when only steps are known
    int millivolts = 0;     // >0 if read directly from the device
    bool charging = false;
    bool connected = true;  // false when the receiver is plugged in but the device is off / out of range
    bool asleep = false;    // true when the device is in idle sleep and the shown % is the last reading
    int graceSec = 60;      // how long to keep the last reading when a poll comes back empty
    unsigned short vid = 0; // USB / Bluetooth vendor id when known (for brand colouring)
};

Link InferLink(const std::wstring& via);
Level InferLevel(const std::wstring& levelText);
const wchar_t* LinkText(Link l);
const wchar_t* LevelText(Level lv);

// Runs all device backends on the caller's thread. Each device is given a short read timeout.
std::vector<DeviceStatus> PollAllDevices();

// Diagnostic dump of every HID collection and serial port the app can see, plus what PollAllDevices() found.
std::wstring DumpHidCollections();

DeviceKind GuessKind(const std::wstring& name, DeviceKind fallback = DeviceKind::Other);
