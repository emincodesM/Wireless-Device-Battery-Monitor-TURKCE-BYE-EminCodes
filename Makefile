CXX      := g++
WINDRES  := windres
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -municode -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
            -finput-charset=UTF-8 -fexec-charset=UTF-8 -ffunction-sections -fdata-sections
LDFLAGS  := -mwindows -municode -s -static-libgcc -static-libstdc++ -Wl,--gc-sections
LIBS     := -lsetupapi -lhid -lcfgmgr32 -lole32 -loleaut32 -luuid -lcomctl32 -luxtheme -ldwmapi -lruntimeobject -lws2_32

BUILD := build
SRCS  := $(wildcard src/*.cpp) $(wildcard src/drivers/*.cpp)
OBJS  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS)) $(BUILD)/app_res.o

OUT := WirelessBattery.exe

all: $(OUT)

$(OUT): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/app_res.o: res/app.rc res/app.manifest res/app.ico res/version.h $(wildcard lang/*.txt)
	@mkdir -p $(BUILD)
	$(WINDRES) -i res/app.rc -o $@

TEST_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
                 -finput-charset=UTF-8 -fexec-charset=UTF-8 -Isrc

$(BUILD)/test_rapoo_protocol.exe: tests/test_rapoo_protocol.cpp src/drivers/rapoo_protocol.h \
                                  src/drivers/atk_serial_protocol.h src/drivers/hyperx_protocol.h \
                                  src/drivers/atk_nordic54_protocol.h src/drivers/galaxy_buds_protocol.h \
                                  src/drivers/logitech_g435_0adf.h src/drivers/ardor_fury_v2_protocol.h \
                                  src/drivers/xenta_protocol.h src/drivers/xenta_protocol.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(TEST_CXXFLAGS) tests/test_rapoo_protocol.cpp src/drivers/xenta_protocol.cpp -o $@ -static-libgcc -static-libstdc++

$(BUILD)/test_rapoo_driver.exe: tests/test_rapoo_driver.cpp src/drivers/rapoo.cpp src/drivers/rapoo_protocol.h \
                                src/drivers/drivers.h src/devices.h src/devices.cpp src/hid.h \
                                src/battery_model.h src/battery_model.cpp src/i18n.h src/i18n.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(TEST_CXXFLAGS) tests/test_rapoo_driver.cpp src/drivers/rapoo.cpp src/battery_model.cpp src/i18n.cpp -o $@ -static-libgcc -static-libstdc++

test: $(BUILD)/test_rapoo_protocol.exe $(BUILD)/test_rapoo_driver.exe
	$(BUILD)/test_rapoo_protocol.exe
	$(BUILD)/test_rapoo_driver.exe

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean test
