@echo off
setlocal
where g++ >nul 2>nul || (
    echo g++ not found on PATH. Install MinGW-w64 ^(e.g. via MSYS2 or WinLibs^) and try again.
    exit /b 1
)

if not exist build mkdir build
windres -i res/app.rc -o build/app_res.o || exit /b 1

g++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -municode -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -ffunction-sections -fdata-sections ^
    src/main.cpp src/flyout.cpp src/widget.cpp src/tray.cpp src/settings.cpp src/battery_model.cpp ^
    src/brands.cpp src/labels.cpp src/i18n.cpp src/devices.cpp src/hid.cpp ^
    src/drivers/*.cpp ^
    build/app_res.o ^
    -o WirelessBattery.exe ^
    -mwindows -municode -s -static-libgcc -static-libstdc++ -Wl,--gc-sections ^
    -lsetupapi -lhid -lcfgmgr32 -lole32 -loleaut32 -luuid -lcomctl32 -luxtheme -ldwmapi -lruntimeobject -lws2_32 || exit /b 1

echo Built WirelessBattery.exe
