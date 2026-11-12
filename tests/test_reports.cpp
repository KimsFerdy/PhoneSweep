#include "report/Exporter.h"
#include "scan/Categorize.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream>

using namespace ps;

namespace {

struct Fixture {
    FileStore files;
    std::vector<AppInfo> apps;
    VerifiedIndex verified;
    ReportInputs in;

    Fixture()
    {
        files.SetVolumes({ { "/storage/emulated/0/", "Internal", false, 64'000'000'000ull, 60'000'000'000ull, 4'000'000'000ull },
            { "/storage/0000-0000/", "SD card", true, 16'000'000'000ull, 11'000'000'000ull, 5'000'000'000ull } });
        std::vector<FileEntry> b(4);
        b[0].path = "/storage/emulated/0/DCIM/Camera/big|pipe.mp4";
        b[0].size = 3'000'000'000;
        b[1].path = "/storage/emulated/0/WhatsApp/Media/WhatsApp Images/IMG-1.jpg";
        b[1].size = 200'000;
        b[2].path = "/storage/0000-0000/Music/song.mp3";
        b[2].size = 5'000'000;
        b[2].volume = 1;
        b[3].path = "/storage/emulated/0/Android/data/x/cache.bin";
        b[3].size = 9'000'000'000; // protected: must not appear in top files
        for (auto& e : b) {
            e.mtime = 1'700'000'000;
            Classify(e, e.volume ? "/storage/0000-0000/" : "/storage/emulated/0/");
        }
        files.Append(std::move(b));

        AppInfo evil;
        evil.package = "com.evil.helper";
        evil.flags = AppFlag_Accessibility | AppFlag_Sideloaded;
        evil.risk = 45;
        AppInfo good;
        good.package = "com.whatsapp";
        good.label = "WhatsApp";
        good.installer = "com.android.vending";
        AppInfo sys;
        sys.package = "com.android.systemui";
        sys.system = true;
        apps = { good, evil, sys };

        in.device.displayName = "Samsung SM-G960F";
        in.device.model = "SM-G960F";
        in.device.serial = "R38K40FAKE01";
        in.device.android = "9";
        in.files = &files;
        in.apps = &apps;
        in.verified = &verified;
        in.now = 1'794'398'400;
    }
};

} // namespace

TEST_CASE("MdCell escapes pipes, control chars and invalid UTF-8")
{
    CHECK(MdCell("a|b") == "a\\|b");
    CHECK(MdCell("line\nbreak") == "line break");
    CHECK(MdCell("Éloko 😀") == "Éloko 😀");
    CHECK(MdCell("bad\xFF") == "bad\xEF\xBF\xBD");
}

TEST_CASE("scan_report.md has every section and the right content")
{
    Fixture f;
    const std::string md = FormatScanReport(f.in);
    for (const char* section : { "# PhoneSweep scan report", "## Device", "## Storage", "## Files by category", "## Files by source",
             "## Top 50 largest files", "## Top folders by size", "## Apps", "### Apps with high-risk access", "## Scan notes" })
        CHECK(md.find(section) != std::string::npos);
    CHECK(md.find("R38K40FAKE01") == std::string::npos); // serial masked by default
    CHECK(md.find("KE01") != std::string::npos);
    CHECK(md.find("big\\|pipe.mp4") != std::string::npos);       // table-safe
    CHECK(md.find("cache.bin") == std::string::npos);            // protected files aren't in the top list
    CHECK(md.find("SD card: Music/song.mp3") != std::string::npos);
    CHECK(md.find("| ACCESSIBILITY | high | 1 | com.evil.helper |") != std::string::npos);
    CHECK(md.find("com.android.systemui") == std::string::npos); // system apps not listed as risky
    CHECK(md.find("1 user apps") == std::string::npos);          // "2 user apps, 1 system apps"
    CHECK(md.find("2 user apps, 1 system apps") != std::string::npos);
}

TEST_CASE("applist.txt: user apps only, riskiest first")
{
    Fixture f;
    const std::string list = FormatUserAppList(f.in);
    CHECK(list.find("package: com.android.systemui") == std::string::npos);
    const auto evil = list.find("package: com.evil.helper");
    const auto wa = list.find("package: com.whatsapp");
    REQUIRE(evil != std::string::npos);
    REQUIRE(wa != std::string::npos);
    CHECK(evil < wa);
}

TEST_CASE("ExportReports writes the folder, masks the serial in logs, and is listed")
{
    Fixture f;
    const auto root = std::filesystem::temp_directory_path() / "ps_reports_test";
    std::filesystem::remove_all(root);
    const auto log = std::filesystem::temp_directory_path() / "ps_session_test.log";
    {
        std::ofstream out(log);
        out << "adb -s R38K40FAKE01 shell ls -> exit 0\n";
    }
    f.in.sessionLog = log;
    const auto r = ExportReports(f.in, root, Export_All);
    REQUIRE(r.ok);
    CHECK(std::filesystem::exists(r.dir / "applist.txt"));
    CHECK(std::filesystem::exists(r.dir / "scan_report.md"));
    CHECK(std::filesystem::exists(r.dir / "export.json"));
    std::string text;
    {
        std::ifstream copied(r.dir / "logs" / log.filename()); // closed before cleanup below
        text.assign((std::istreambuf_iterator<char>(copied)), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("R38K40FAKE01") == std::string::npos);
    CHECK(text.find("KE01 shell ls") != std::string::npos);

    const auto list = ListExports(root);
    REQUIRE(list.size() == 1);
    CHECK(list[0].device == "Samsung SM-G960F");
    CHECK(list[0].files.size() == 3);
    CHECK(list[0].bytes > 0);
    std::filesystem::remove_all(root);
    std::filesystem::remove(log);
}
