#include "compress/M8SelfTest.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "app/Settings.h"
#include "compress/Compress.h"
#include "nuke/Nuker.h"
#include "restore/Restore.h"
#include "thumbs/Exif.h"
#include "thumbs/ImageOps.h"
#include "transfer/Manifest.h"
#include "transfer/PhoneIO.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Process.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <format>
#include <fstream>
#include <thread>

namespace ps {

namespace {

constexpr int64_t kMtime = 1600000000; // 2020-09-13: easy to spot if it's lost

// A "camera" JPEG: textured but compressible, quality 100, with an EXIF block (orientation 6).
std::string MakePhoto(int w, int h, uint32_t seed)
{
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1103515245 + 12345;
            const int n = static_cast<int>((seed >> 16) & 31);
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>((x * 255 / w + n) & 255);
            p[1] = static_cast<uint8_t>((y * 255 / h + n) & 255);
            p[2] = static_cast<uint8_t>(((x ^ y) & 255) / 2 + n);
            p[3] = 255;
        }
    const std::string plain = EncodeJpeg(img, 100);
    const unsigned char tiff[] = { 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0 };
    std::string app1 = "\xFF\xE1";
    const size_t len = 2 + 6 + sizeof(tiff);
    app1.push_back(static_cast<char>(len >> 8));
    app1.push_back(static_cast<char>(len & 0xFF));
    app1.append("Exif\0\0", 6);
    app1.append(reinterpret_cast<const char*>(tiff), sizeof(tiff));
    return plain.substr(0, 2) + app1 + plain.substr(2);
}

bool WriteAll(const std::filesystem::path& p, std::string_view data)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

} // namespace

int RunM8SelfTest()
{
    const auto exe = AdbClient::Locate();
    if (!exe) {
        LogError("m8 selftest: adb not found");
        return 2;
    }
    const AdbClient adb(*exe);
    std::string serial;
    for (const auto& d : ParseDevicesList(adb.Run({ "devices", "-l" }).out))
        if (d.state == DeviceEntry::State::Device)
            serial = d.serial;
    if (serial.empty()) {
        LogError("m8 selftest: no authorized device");
        return 3;
    }
    const auto ffmpeg = FindExecutable(L"ffmpeg.exe");
    if (!ffmpeg) {
        LogError("m8 selftest: ffmpeg not found");
        return 4;
    }

    int failures = 0;
    auto check = [&](bool ok, std::string_view what) {
        if (ok)
            LogInfo("m8 selftest: OK   {}", what);
        else {
            LogError("m8 selftest: FAIL {}", what);
            ++failures;
        }
    };
    auto stat = [&](const std::string& path) -> std::pair<uint64_t, int64_t> {
        const auto r = adb.Shell(serial, ShellCommand("stat").arg("-c").arg("%s %Y").arg(path).raw("2>/dev/null"));
        const auto parts = SplitWhitespace(Trim(r.out));
        if (parts.size() != 2)
            return { 0, 0 };
        return { ParseInt<uint64_t>(parts[0]).value_or(0), ParseInt<int64_t>(parts[1]).value_or(0) };
    };

    const std::string base = "/storage/emulated/0/PhoneSweepSelfTest/m8/";
    const auto tmp = Settings::DataDir() / L"m8selftest";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp / L"backup", ec);
    adb.Shell(serial, ShellCommand("rm").arg("-rf").arg(base));
    std::stop_source ss;
    const auto st = ss.get_token();

    // 1. Push (PushVerified): photo, second photo, high-bitrate .mov video.
    const std::string photo = MakePhoto(2400, 1800, 7), photo2 = MakePhoto(2000, 1500, 99);
    WriteAll(tmp / L"photo.jpg", photo);
    WriteAll(tmp / L"backup" / L"photo2.jpg", photo2);
    {
        const std::vector<std::string> args = { "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=30", "-f",
            "lavfi", "-i", "sine=frequency=440", "-t", "6", "-c:v", "libx264", "-preset", "ultrafast", "-qp", "4", "-pix_fmt", "yuv420p", "-c:a", "aac",
            "-shortest", Narrow((tmp / L"clip.mov").wstring()) };
        const auto r = RunProcess(*ffmpeg, args, { .timeout = std::chrono::minutes(2), .logName = "ffmpeg" });
        check(r.ok(), "generate test video");
    }
    const uint64_t clipSize = std::filesystem::file_size(tmp / L"clip.mov", ec);
    LogInfo("m8 selftest: photo {} B, photo2 {} B, clip {} B", photo.size(), photo2.size(), clipSize);
    check(PushVerified(adb, serial, tmp / L"photo.jpg", base + "photo.jpg", Md5Hex(photo), kMtime, st).empty(), "push photo.jpg");
    check(PushVerified(adb, serial, tmp / L"backup" / L"photo2.jpg", base + "photo2.jpg", Md5Hex(photo2), kMtime, st).empty(), "push photo2.jpg");
    check(PushVerified(adb, serial, tmp / L"clip.mov", base + "clip.mov", *Md5File(tmp / L"clip.mov"), kMtime, st).empty(), "push clip.mov");
    check(stat(base + "photo.jpg") == std::pair<uint64_t, int64_t>(photo.size(), kMtime), "photo.jpg size + mtime on the phone");
    check(stat(base + "clip.mov") == std::pair<uint64_t, int64_t>(clipSize, kMtime), "clip.mov size + mtime on the phone");

    // 2. MediaStore knows them after a scan (best effort: indexing is async).
    MediaScan(adb, serial, { base + "photo.jpg", base + "photo2.jpg", base + "clip.mov" }, st);
    // (Samsung's Android 9 scanner files .mov as a non-media file, so the video is checked
    // after it has become clip.mp4 below; camera videos are .mp4 anyway.)
    auto queryMedia = [&](const std::string& want) {
        std::map<std::string, MediaInfo, std::less<>> media;
        for (int attempt = 0; attempt < 15 && !media.contains(want); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            media.clear();
            for (const char* uri : { "content://media/external/video/media", "content://media/external/images/media" }) {
                const auto r = adb.Shell(serial, ShellCommand("content").arg("query").arg("--uri").arg(uri).arg("--projection")
                                                     .arg(std::string_view(uri).find("video") != std::string_view::npos ? "_data:duration:width:height:_size"
                                                                                                                        : "_data:width:height:_size")
                                                     .arg("--where").arg("_data LIKE '%PhoneSweepSelfTest/m8/%'"),
                    { .timeout = 30s });
                media.merge(ParseMediaInfoRows(r.out));
            }
        }
        return media;
    };
    {
        const auto media = queryMedia(base + "photo.jpg");
        check(media.contains(base + "photo.jpg") && media.at(base + "photo.jpg").width == 2400, "MediaStore: photo size");
    }
    const MediaInfo clipInfo{ 6000, 1920, 1080 };

    auto candidate = [&](const std::string& path, MediaKind kind, uint64_t size, MediaInfo info) {
        CompressCandidate c;
        c.path = path;
        c.kind = kind;
        c.size = size;
        c.mtime = kMtime;
        c.info = info;
        return c;
    };
    const std::vector<std::string> roots = { "/storage/emulated/0/" }, labels = { "Internal" };
    auto run = [&](std::vector<CompressCandidate> items, CompressOutcome outcome) {
        CompressTask t;
        t.serial = serial;
        t.items = std::move(items);
        t.outcome = outcome;
        t.ffmpeg = *ffmpeg;
        t.useQsv = ProbeQsv(*ffmpeg, tmp);
        t.pcFolder = tmp / L"pc";
        t.volumeRoots = roots;
        t.volumeLabels = labels;
        t.tmpDir = tmp / L"work";
        CompressProgress p;
        RunCompress(adb, t, p, st);
        for (const auto& r : p.results)
            LogInfo("m8 selftest: {} -> {} : {} ({} -> {} B)", r.path, r.newPath, r.status, r.before, r.after);
        return std::move(p.results);
    };

    // 3. Photo, phone only: replaced in place, smaller, same date, EXIF kept.
    {
        const auto res = run({ candidate(base + "photo.jpg", MediaKind::Photo, photo.size(), { 0, 2400, 1800 }) }, CompressOutcome::PhoneOnly);
        check(res.size() == 1 && res[0].ok, "compress photo (phone only)");
        const auto [size, mtime] = stat(base + "photo.jpg");
        check(size > 0 && size < photo.size() && mtime == kMtime, "photo smaller on the phone, date kept");
        const auto pulled = PullToFile(adb, serial, base + "photo.jpg", tmp / L"photo_after.jpg", st);
        std::ifstream in(tmp / L"photo_after.jpg", std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(pulled.ok && ParseJpegExif({ reinterpret_cast<const uint8_t*>(data.data()), data.size() }).orientation == 6, "photo EXIF orientation kept");
    }
    // 4. Video .mov, phone + PC: becomes clip.mp4 on the phone, copy on the PC, .mov gone.
    {
        const auto res = run({ candidate(base + "clip.mov", MediaKind::Video, clipSize, clipInfo) }, CompressOutcome::PhoneAndPc);
        check(res.size() == 1 && res[0].ok, "compress video (phone + PC)");
        const auto [size, mtime] = stat(base + "clip.mp4");
        check(size > 0 && size < clipSize && mtime == kMtime, "clip.mp4 on the phone, smaller, date kept");
        check(stat(base + "clip.mov").first == 0, "clip.mov removed after rename");
        bool pcCopy = false;
        for (auto it = std::filesystem::recursive_directory_iterator(tmp / L"pc", ec); !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
            pcCopy |= it->path().filename() == L"clip.mp4" && it->file_size() == size;
        check(pcCopy, "PC copy of clip.mp4 matches");
        const auto media = queryMedia(base + "clip.mp4");
        const auto it = media.find(base + "clip.mp4");
        check(it != media.end() && it->second.width == 1920 && it->second.durationMs >= 5000, "MediaStore: clip.mp4 indexed with duration + size");
    }
    // 5. Photo2, PC only: removed from the phone.
    {
        const auto res = run({ candidate(base + "photo2.jpg", MediaKind::Photo, photo2.size(), { 0, 2000, 1500 }) }, CompressOutcome::PcOnly);
        check(res.size() == 1 && res[0].ok, "compress photo2 (PC only)");
        check(stat(base + "photo2.jpg").first == 0, "photo2.jpg removed from the phone");
    }
    // 6. Restore: a backup holding the ORIGINAL photo2 (missing now) and photo.jpg (different now).
    {
        Manifest m(tmp / L"backup" / L"saved_manifest.json", serial, "selftest");
        ManifestEntry e;
        e.devicePath = base + "photo2.jpg";
        e.size = photo2.size();
        e.mtime = kMtime;
        e.md5 = Md5Hex(photo2);
        e.localRel = "photo2.jpg";
        m.Upsert(e);
        e.devicePath = base + "photo.jpg";
        e.size = photo.size();
        e.md5 = Md5Hex(photo);
        e.localRel = "photo.jpg";
        std::filesystem::copy_file(tmp / L"photo.jpg", tmp / L"backup" / L"photo.jpg", ec);
        m.Upsert(e);
        check(m.Save(), "write test manifest");
        const auto folders = ListBackupFolders(tmp);
        check(folders.size() == 1 && folders[0].files == 2, "ListBackupFolders finds it");
        auto items = PlanRestore(adb, serial, tmp / L"backup", roots, st);
        size_t missing = 0, different = 0;
        for (const auto& it : items) {
            missing += it.state == RestoreState::Missing;
            different += it.state == RestoreState::Different;
        }
        check(items.size() == 2 && missing == 1 && different == 1, "plan: 1 missing, 1 different");
        RestoreProgress p;
        RunRestore(adb, serial, items, RestorePolicy::SkipExisting, p, st);
        check(p.restored == 1 && p.skipped == 1 && p.failed == 0, "restore: 1 restored, 1 skipped");
        check(stat(base + "photo2.jpg") == std::pair<uint64_t, int64_t>(photo2.size(), kMtime), "photo2.jpg back with its date");
        check(PhoneMd5(adb, serial, base + "photo2.jpg", st) == Md5Hex(photo2), "photo2.jpg MD5 matches the backup");
        RestoreProgress p2;
        RunRestore(adb, serial, items, RestorePolicy::KeepBoth, p2, st);
        check(stat(base + "photo (restored).jpg").first == photo.size(), "keep both: 'photo (restored).jpg'");
    }

    // 7. Clean up everything we made (phone + gallery + PC).
    std::vector<std::string> all;
    for (const char* n : { "photo.jpg", "photo2.jpg", "clip.mov", "clip.mp4", "photo (restored).jpg" })
        all.push_back(base + n);
    all.push_back("/storage/emulated/0/PhoneSweepSelfTest/m8"); // folder rows too
    all.push_back("/storage/emulated/0/PhoneSweepSelfTest");
    adb.Shell(serial, ShellCommand("rm").arg("-rf").arg("/storage/emulated/0/PhoneSweepSelfTest"));
    CleanMediaStore(adb, serial, all, st);
    std::filesystem::remove_all(tmp, ec);

    LogInfo("m8 selftest: {}", failures == 0 ? "PASS" : std::format("{} FAILURE(S)", failures));
    return failures == 0 ? 0 : 1;
}

} // namespace ps
