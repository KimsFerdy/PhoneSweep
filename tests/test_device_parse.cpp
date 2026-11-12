#include "adb/Device.h"

#include <doctest/doctest.h>

using namespace ps;

TEST_CASE("ParseDevicesList: authorized, unauthorized, offline")
{
    const char* text = "List of devices attached\r\n"
                       "R38K40FAKE01   device product:starltexx model:SM_G960F device:starlte transport_id:1\r\n"
                       "R5CW30A1B2C3           unauthorized usb:1-1 transport_id:2\r\n"
                       "192.168.0.20:5555      offline transport_id:3\r\n"
                       "\r\n";
    const auto list = ParseDevicesList(text);
    REQUIRE(list.size() == 3);
    CHECK(list[0].serial == "R38K40FAKE01");
    CHECK(list[0].state == DeviceEntry::State::Device);
    CHECK(list[0].model == "SM-G960F");
    CHECK(list[0].product == "starltexx");
    CHECK(list[0].transportId == "1");
    CHECK(list[1].state == DeviceEntry::State::Unauthorized);
    CHECK(list[1].model.empty());
    CHECK(list[2].state == DeviceEntry::State::Offline);
    CHECK(list[2].serial == "192.168.0.20:5555");
}

TEST_CASE("ParseDevicesList: daemon chatter and empty list")
{
    const char* text = "* daemon not running; starting now at tcp:5037\n"
                       "* daemon started successfully\n"
                       "List of devices attached\n\n";
    CHECK(ParseDevicesList(text).empty());
    CHECK(ParseDevicesList("").empty());
}

TEST_CASE("ParseDevicesList: recovery / sideload map to Other")
{
    const auto list = ParseDevicesList("List of devices attached\nABC123 recovery\n");
    REQUIRE(list.size() == 1);
    CHECK(list[0].state == DeviceEntry::State::Other);
    CHECK(list[0].rawState == "recovery");
}

TEST_CASE("ParseGetprop")
{
    const auto p = ParseGetprop("[ro.product.model]: [SM-G960F]\n"
                                "[ro.build.version.release]: [9]\n"
                                "[empty.prop]: []\n"
                                "[with.brackets]: [a]: [b]\n"
                                "garbage line\n");
    CHECK(p.at("ro.product.model") == "SM-G960F");
    CHECK(p.at("ro.build.version.release") == "9");
    CHECK(p.at("empty.prop").empty());
    CHECK(p.at("with.brackets") == "a]: [b");
    CHECK(p.size() == 4);
}

TEST_CASE("DecodeOneUi")
{
    CHECK(DecodeOneUi("60100", "") == "6.1");
    CHECK(DecodeOneUi("50101", "") == "5.1.1");
    CHECK(DecodeOneUi("", "100000") == "1.0"); // One UI 1.0 on Android 9
    CHECK(DecodeOneUi("", "100500") == "1.5");
    CHECK(DecodeOneUi("", "110000") == "2.0");
    CHECK(DecodeOneUi("", "90500").empty()); // Samsung Experience, not One UI
    CHECK(DecodeOneUi("", "").empty());
}

TEST_CASE("ParseDumpsysBattery ignores Samsung's trailing blocks")
{
    const char* text = "Current Battery Service state:\n"
                       "  AC powered: false\n"
                       "  USB powered: true\n"
                       "  status: 2\n"
                       "  level: 64\n"
                       "  temperature: 312\n"
                       "USE_FAKE_BATTERY: false\n"
                       "BatteryInfoBackUp\n"
                       "  level: 99\n"
                       "  temperature: 582\n";
    const auto b = ParseDumpsysBattery(text);
    REQUIRE(b.level);
    CHECK(*b.level == 64);
    CHECK(b.charging);
    REQUIRE(b.tempC);
    CHECK(*b.tempC == doctest::Approx(31.2));
}

TEST_CASE("ParseDumpsysBattery: full (status 5) is not charging")
{
    const auto b = ParseDumpsysBattery("Current Battery Service state:\n  status: 5\n  level: 100\n");
    CHECK_FALSE(b.charging);
    CHECK(*b.level == 100);
}

TEST_CASE("MemTotal and marketing RAM")
{
    CHECK(ParseMemTotalKb("MemTotal:        5339752 kB\nMemFree: 1 kB\n") == 5339752u);
    CHECK_FALSE(ParseMemTotalKb("nothing").has_value());
    CHECK(MarketingRamGB(5339752) == 6);  // 5.3 GB reported -> 6 GB phone
    CHECK(MarketingRamGB(7604000) == 8);  // typical 8 GB phone
    CHECK(MarketingRamGB(11500000) == 12);
    CHECK(MarketingRamGB(3800000) == 4);
}

TEST_CASE("ParseDfK")
{
    const char* text = "Filesystem      1K-blocks     Used Available Use% Mounted on\n"
                       "/dev/block/dm-1  55456988 53056760   2264036  96% /data\n";
    const auto s = ParseDfK(text);
    REQUIRE(s);
    CHECK(s->total == 55456988ull * 1024);
    CHECK(s->used == 53056760ull * 1024);
    CHECK(s->free == 2264036ull * 1024);
    CHECK(s->UsedFraction() == doctest::Approx(0.9567).epsilon(0.001));
    CHECK_FALSE(ParseDfK("df: /data: Permission denied\n").has_value());
}

TEST_CASE("MaskSerial keeps last 4")
{
    CHECK(MaskSerial("R5CW30A1B2C3") == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" "B2C3");
    CHECK(MaskSerial("ABCD") == "ABCD");
}
