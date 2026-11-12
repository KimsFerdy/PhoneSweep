#include "bg/Background.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <ostream>

using namespace ps;

TEST_CASE("ParseBgBatch: default vs explicit appops, buckets")
{
    const char* text = "@@org.kde.kdeconnect_tp\n"
                       "No operations.\n"
                       "Default mode: allow\n"
                       "20\n"
                       "@@com.restricted\n"
                       "RUN_ANY_IN_BACKGROUND: ignore; time=+3m12s ago\n"
                       "Default mode: allow\n"
                       "40\n"
                       "@@com.reset\n"
                       "RUN_ANY_IN_BACKGROUND: default\n"
                       "10\n";
    const auto m = ParseBgBatch(text);
    REQUIRE(m.size() == 3);
    const auto& kde = m.at("org.kde.kdeconnect_tp");
    CHECK(kde.mode == "allow");
    CHECK_FALSE(kde.explicitMode);
    CHECK(kde.bucket == 20);
    CHECK(kde.BackgroundAllowed());

    const auto& r = m.at("com.restricted");
    CHECK(r.mode == "ignore"); // explicit wins over the "Default mode" line after it
    CHECK(r.explicitMode);
    CHECK(r.bucket == 40);
    CHECK_FALSE(r.BackgroundAllowed());

    CHECK(m.at("com.reset").mode == "default");
    CHECK(m.at("com.reset").BackgroundAllowed());
}

TEST_CASE("BucketName")
{
    CHECK(std::string(BucketName(10)) == "Active");
    CHECK(std::string(BucketName(40)) == "Rare");
    CHECK(std::string(BucketName(45)) == "Restricted");
    CHECK(std::string(BucketName(-1)) == "\xE2\x80\x94");
}

TEST_CASE("ParsePsApps keeps app users only and groups by package")
{
    const char* text = "  PID    RSS USER         NAME\n"
                       "    1   1844 root         init\n"
                       " 2014  63300 u0_a1131     com.samsung.android.soundassistant\n"
                       " 3222  83356 u0_a47       com.google.android.apps.turbo:aab\n"
                       " 3223  10000 u0_a47       com.google.android.apps.turbo\n"
                       " 3914 486920 u0_a274      com.zhiliaoapp.musically.go\n"
                       " 4000  20000 u10_a12      com.work.app\n"
                       " 5000  30000 system       system_server\n";
    const auto procs = ParsePsApps(text);
    CHECK(procs.size() == 5);
    const auto apps = GroupByPackage(procs);
    REQUIRE(apps.size() == 4);
    CHECK(apps[0].package == "com.zhiliaoapp.musically.go"); // biggest first
    const auto turbo = std::ranges::find(apps, std::string("com.google.android.apps.turbo"), &RunningApp::package);
    REQUIRE(turbo != apps.end());
    CHECK(turbo->processes == 2);
    CHECK(turbo->rssKb == 93356);
}

TEST_CASE("RestrictionLog persists, keeps the ORIGINAL prior state, and removes")
{
    const auto file = std::filesystem::temp_directory_path() / "ps_test_restrictions.json";
    std::filesystem::remove(file);
    {
        RestrictionLog log(file);
        Restriction r;
        r.serial = "dev1";
        r.package = "com.a";
        r.before = { "allow", false, 10 };
        r.appliedBucket = 40;
        log.Add(r);

        // Restricting again must not overwrite "before" with the restricted state.
        Restriction again = r;
        again.before = { "ignore", true, 40 };
        log.Add(again);
        CHECK(log.Count() == 1);
        CHECK(log.Find("dev1", "com.a")->before.mode == "allow");

        Restriction other = r;
        other.serial = "dev2";
        log.Add(other);
    }
    {
        RestrictionLog log(file); // reload from disk
        CHECK(log.Count() == 2);
        CHECK(log.ForDevice("dev1").size() == 1);
        const auto r = log.Find("dev1", "com.a");
        REQUIRE(r);
        CHECK(r->before.bucket == 10);
        CHECK_FALSE(r->before.explicitMode);
        log.Remove("dev1", "com.a");
        CHECK_FALSE(log.Find("dev1", "com.a"));
    }
    RestrictionLog reloaded(file);
    CHECK(reloaded.Count() == 1);
    std::filesystem::remove(file);
}
