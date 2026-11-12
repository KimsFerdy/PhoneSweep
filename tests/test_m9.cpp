#include "adb/Wireless.h"
#include "backup/Zip.h"
#include "backup/ZipBackup.h"
#include "restore/Restore.h"
#include "transfer/Manifest.h"
#include "util/Md5.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream>

using namespace ps;

TEST_CASE("NormalizeAddress")
{
    CHECK(NormalizeAddress("192.168.0.3") == "192.168.0.3:5555");
    CHECK(NormalizeAddress(" 192.168.0.3:37199 ") == "192.168.0.3:37199");
    CHECK(NormalizeAddress("pixel.local:5555") == "pixel.local:5555");
    CHECK_FALSE(NormalizeAddress(""));
    CHECK_FALSE(NormalizeAddress("192.168.1"));
    CHECK_FALSE(NormalizeAddress("192.168.0.300"));
    CHECK_FALSE(NormalizeAddress("192.168.0.3:0"));
    CHECK_FALSE(NormalizeAddress("192.168.0.3:99999"));
    CHECK_FALSE(NormalizeAddress("http://192.168.0.3"));
}

TEST_CASE("ParseWifiIp prefers wlan, skips loopback and mobile data")
{
    const char* ipDump = "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000\n"
                        "    inet 127.0.0.1/8 scope host lo\n"
                        "       valid_lft forever preferred_lft forever\n"
                        "14: rmnet_data0: <UP,LOWER_UP> mtu 1500\n"
                        "    inet 10.20.30.40/30 scope global rmnet_data0\n"
                        "9: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP group default qlen 1000\n"
                        "    inet 192.168.0.3/24 brd 192.168.0.255 scope global wlan0\n";
    CHECK(ParseWifiIp(ipDump) == "192.168.0.3");
    CHECK(ParseWifiIp("1: lo: <LOOPBACK>\n    inet 127.0.0.1/8 scope host lo\n") == std::nullopt);
    CHECK(ParseWifiIp("5: eth0@if6: <UP>\n    inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0\n") == "10.0.2.15"); // emulator
}

TEST_CASE("adb connect / pair output")
{
    CHECK(ParseConnectOutput("connected to 192.168.0.3:5555\n", "192.168.0.3:5555").ok);
    CHECK(ParseConnectOutput("already connected to 192.168.0.3:5555\n", "192.168.0.3:5555").ok);
    const auto refused = ParseConnectOutput("failed to connect to '192.168.0.3:5555': Connection refused\n", "192.168.0.3:5555");
    CHECK_FALSE(refused.ok);
    CHECK(refused.message.find("refused") != std::string::npos);
    CHECK_FALSE(ParseConnectOutput("cannot connect to 192.168.0.9:5555: A connection attempt failed... (10060)", "x").ok);
    CHECK(ParsePairOutput("Successfully paired to 192.168.0.3:41234 [guid=adb-XYZ]").ok);
    CHECK_FALSE(ParsePairOutput("Failed: Wrong password or connection was dropped.").ok);
}

TEST_CASE("Crc32 known values")
{
    const std::string s = "123456789";
    CHECK(Crc32({ reinterpret_cast<const uint8_t*>(s.data()), s.size() }) == 0xCBF43926u);
    // Incremental == one shot, across the slice-by-8 boundary.
    std::string big(1000, 'x');
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>(i * 31);
    const auto* p = reinterpret_cast<const uint8_t*>(big.data());
    CHECK(Crc32({ p + 13, big.size() - 13 }, Crc32({ p, 13 })) == Crc32({ p, big.size() }));
}

namespace {

std::filesystem::path TempDir(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "phonesweep_tests" / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

void WriteFile(const std::filesystem::path& p, const std::string& data)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
}

} // namespace

TEST_CASE("ZipWriter/ZipReader round trip (UTF-8 names, CRC check)")
{
    const auto dir = TempDir("zip_rt");
    WriteFile(dir / "a.bin", std::string(3'000'000, 'a'));
    const std::string emoji = "DCIM/\xF0\x9F\x98\x80 caf\xC3\xA9.jpg";
    {
        ZipWriter w;
        std::string err;
        REQUIRE(w.Open(dir / "out.zip", &err));
        REQUIRE(w.AddBytes("saved_manifest.json", "{}", 1600000000, &err));
        REQUIRE(w.AddFile("big/a.bin", dir / "a.bin", 1600000000, {}, {}, &err));
        REQUIRE(w.AddBytes(emoji, "hello", 0, &err));
        REQUIRE(w.Close(&err));
    }
    std::string err;
    uint64_t bigOffset = 0;
    {
        ZipReader r; // closed before the file is modified below (readers deny writes)
        REQUIRE(r.Open(dir / "out.zip", &err));
        REQUIRE(r.Entries().size() == 3);
        REQUIRE(r.Find(emoji));
        CHECK(r.ReadAll(*r.Find(emoji), &err) == "hello");
        const auto* big = r.Find("big/a.bin");
        REQUIRE(big);
        CHECK(big->size == 3'000'000);
        bigOffset = big->localOffset;
        REQUIRE(r.Extract(*big, dir / "a.out", {}, &err));
        CHECK(std::filesystem::file_size(dir / "a.out") == 3'000'000);
    }

    // Flip one byte inside a.bin's data: the CRC check must catch it.
    {
        std::fstream f(dir / "out.zip", std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(f.good());
        f.seekp(static_cast<std::streamoff>(bigOffset + 30 + 9 + 20 + 1000));
        f.put('b');
    }
    ZipReader r2;
    REQUIRE(r2.Open(dir / "out.zip", &err));
    CHECK_FALSE(r2.Extract(*r2.Find("big/a.bin"), dir / "a.out", {}, &err));
    CHECK(err.find("CRC") != std::string::npos);
}

TEST_CASE("Windows can read our zip (tar.exe)")
{
    const auto dir = TempDir("zip_tar");
    {
        ZipWriter w;
        std::string err;
        REQUIRE(w.Open(dir / "t.zip", &err));
        REQUIRE(w.AddBytes("folder/hello.txt", "hi there", 1600000000, &err));
        REQUIRE(w.Close(&err));
    }
    std::filesystem::create_directories(dir / "x");
    const std::string cmd = "tar -xf \"" + (dir / "t.zip").string() + "\" -C \"" + (dir / "x").string() + "\" 2>nul";
    if (std::system(cmd.c_str()) == 0) { // bsdtar ships with Windows 10+
        std::ifstream in(dir / "x" / "folder" / "hello.txt");
        std::string s;
        std::getline(in, s);
        CHECK(s == "hi there");
    }
}

TEST_CASE("ZipBackupFolder: verified zip, manifest inside, restore plan reads it, verified index sees it")
{
    const auto root = TempDir("zip_backup");
    const auto folder = root / "SM-TEST_2026-11-12";
    const std::string photo(200'000, 'p'), doc = "notes";
    WriteFile(folder / "DCIM" / "Camera" / "p.jpg", photo);
    WriteFile(folder / "Documents" / "n.txt", doc);
    {
        Manifest m(folder / "saved_manifest.json", "SERIAL1", "SM-TEST");
        ManifestEntry e;
        e.devicePath = "/storage/emulated/0/DCIM/Camera/p.jpg";
        e.size = photo.size();
        e.mtime = 1600000000;
        e.md5 = Md5Hex(photo);
        e.localRel = "DCIM/Camera/p.jpg";
        m.Upsert(e);
        e.devicePath = "/storage/emulated/0/Documents/n.txt";
        e.size = doc.size();
        e.md5 = Md5Hex(doc);
        e.localRel = "Documents/n.txt";
        m.Upsert(e);
        REQUIRE(m.Save());
    }

    ZipProgress p;
    ZipBackupFolder(folder, /*deleteFolder*/ true, p, {});
    REQUIRE(p.finished);
    REQUIRE(p.ok);
    CHECK(p.folderRemoved);
    CHECK_FALSE(std::filesystem::exists(folder));
    CHECK(p.zip.filename() == "SM-TEST_2026-11-12.zip");
    CHECK(p.files == 3);

    const auto zm = ReadZipManifest(p.zip);
    REQUIRE(zm);
    CHECK(zm->serial == "SERIAL1");
    CHECK(zm->entries.size() == 2);

    const auto list = ListBackupFolders(root);
    REQUIRE(list.size() == 1);
    CHECK(list[0].zip);
    CHECK(list[0].files == 2);
    CHECK(list[0].model == "SM-TEST");

    VerifiedIndex vi;
    CHECK(vi.LoadForDevice(root, "SERIAL1") == 2);
    CHECK(vi.IsVerified("/storage/emulated/0/DCIM/Camera/p.jpg", photo.size(), 1600000000));
    CHECK(vi.LocalFile("/storage/emulated/0/DCIM/Camera/p.jpg") == p.zip);
    CHECK(vi.LoadForDevice(root, "OTHER") == 0);
}

TEST_CASE("ZipBackupFolder refuses a folder without a manifest and leaves it alone")
{
    const auto root = TempDir("zip_nomanifest");
    WriteFile(root / "random" / "x.txt", "x");
    ZipProgress p;
    ZipBackupFolder(root / "random", true, p, {});
    CHECK(p.finished);
    CHECK_FALSE(p.ok);
    CHECK(std::filesystem::exists(root / "random" / "x.txt"));
    CHECK_FALSE(std::filesystem::exists(root / "random.zip"));
}
