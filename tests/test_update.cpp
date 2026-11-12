#include "update/Update.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace ps;

TEST_CASE("ParseSemVer and ordering")
{
    CHECK(ParseSemVer("v1.2.3") == SemVer{ 1, 2, 3 });
    CHECK(ParseSemVer("1.10.0") == SemVer{ 1, 10, 0 });
    CHECK(ParseSemVer("2.0") == SemVer{ 2, 0, 0 });
    CHECK(ParseSemVer("v1.1.0-beta.2") == SemVer{ 1, 1, 0 });
    CHECK_FALSE(ParseSemVer("latest"));
    CHECK_FALSE(ParseSemVer("1"));
    CHECK_FALSE(ParseSemVer("1.x.0"));
    CHECK_FALSE(ParseSemVer(""));
    CHECK(SemVer{ 1, 10, 0 } > SemVer{ 1, 9, 9 });
    CHECK(SemVer{ 2, 0, 0 } > SemVer{ 1, 99, 0 });
    CHECK_FALSE(SemVer{ 1, 1, 0 } > SemVer{ 1, 1, 0 });
    CHECK(SemVer{ 1, 1, 0 }.Str() == "1.1.0");
}

TEST_CASE("ParseRelease reads GitHub's latest-release JSON")
{
    const char* json = R"({
      "tag_name": "v1.1.0", "name": "PhoneSweep v1.1.0", "body": "## What's new\n- Compare duplicates",
      "html_url": "https://github.com/KimsFerdy/PhoneSweep/releases/tag/v1.1.0", "published_at": "2026-11-20T10:00:00Z",
      "assets": [
        { "name": "PhoneSweep-1.1.0-setup.exe", "size": 100, "digest": "sha256:ABCDEF0000000000000000000000000000000000000000000000000000000001",
          "browser_download_url": "https://github.com/KimsFerdy/PhoneSweep/releases/download/v1.1.0/PhoneSweep-1.1.0-setup.exe" },
        { "name": "PhoneSweep-1.1.0-x64.msi", "size": 200, "digest": null,
          "browser_download_url": "https://github.com/KimsFerdy/PhoneSweep/releases/download/v1.1.0/PhoneSweep-1.1.0-x64.msi" },
        { "name": "PhoneSweep-1.1.0-win64.zip", "size": 300,
          "browser_download_url": "https://github.com/KimsFerdy/PhoneSweep/releases/download/v1.1.0/PhoneSweep-1.1.0-win64.zip" },
        { "name": "evil-setup.exe", "size": 1, "browser_download_url": "https://example.com/evil-setup.exe" }
      ]})";
    std::string err;
    const auto r = ParseRelease(json, &err);
    REQUIRE(r);
    CHECK(r->version == SemVer{ 1, 1, 0 });
    CHECK(r->title == "PhoneSweep v1.1.0");
    CHECK(r->publishedAt == "2026-11-20");
    CHECK(r->notes.starts_with("## What's new"));
    REQUIRE(r->assets.size() == 3); // the foreign URL is dropped
    CHECK(r->assets[0].sha256 == "abcdef0000000000000000000000000000000000000000000000000000000001");
    CHECK(r->assets[1].sha256.empty());

    CHECK(PickAsset(*r, InstallKind::InnoUser)->name == "PhoneSweep-1.1.0-setup.exe");
    CHECK(PickAsset(*r, InstallKind::InnoAllUsers)->name == "PhoneSweep-1.1.0-setup.exe");
    CHECK(PickAsset(*r, InstallKind::Msi)->size == 200);
    CHECK(PickAsset(*r, InstallKind::Portable)->name == "PhoneSweep-1.1.0-win64.zip");

    CHECK_FALSE(ParseRelease("not json", &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(ParseRelease(R"({"tag_name":"nightly"})"));
}

TEST_CASE("FormatReleaseNotes flattens GitHub Markdown")
{
    const auto n = FormatReleaseNotes("## What's new\r\n\r\n- **Compare** duplicates `side by side`\n* see [the docs](https://x.y)\n\n\n"
                                      "| File | Best for |\n|---|---|\n| `setup.exe` | most people |\nPlain text\n\n");
    REQUIRE(n.size() == 8);
    CHECK(n[0].kind == NoteLine::Kind::Heading);
    CHECK(n[0].text == "What's new");
    CHECK(n[1].kind == NoteLine::Kind::Blank);
    CHECK(n[2].kind == NoteLine::Kind::Bullet);
    CHECK(n[2].text == "Compare duplicates side by side");
    CHECK(n[3].text == "see the docs");
    CHECK(n[4].kind == NoteLine::Kind::Blank); // several blank lines collapse into one
    CHECK(n[5].text == "File  \xC2\xB7  Best for");
    CHECK(n[6].text == "setup.exe  \xC2\xB7  most people"); // the |---| row is dropped
    CHECK(n[7].text == "Plain text");                       // trailing blanks trimmed
}

TEST_CASE("Sha256")
{
    CHECK(Sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Installer command lines are silent and relaunch PhoneSweep")
{
    const std::filesystem::path setup = LR"(C:\Temp\PhoneSweep-update\PhoneSweep-1.1.0-setup.exe)";
    const auto inno = MakeInstallerLaunch(InstallKind::InnoUser, setup);
    CHECK(inno.exe == setup);
    CHECK(inno.params.find(L"/SILENT") != std::wstring::npos);
    CHECK(inno.params.find(L"/CURRENTUSER") != std::wstring::npos);
    CHECK(inno.params.find(L"/RELAUNCH=1") != std::wstring::npos);
    CHECK(MakeInstallerLaunch(InstallKind::InnoAllUsers, setup).params.find(L"/ALLUSERS") != std::wstring::npos);

    const auto msi = MakeInstallerLaunch(InstallKind::Msi, LR"(C:\Temp\x.msi)");
    CHECK(msi.exe.filename() == L"msiexec.exe");
    const std::wstring msiStart = LR"(/i "C:\Temp\x.msi" /passive)";
    CHECK(msi.params.starts_with(msiStart));
    CHECK(msi.params.find(L"PS_RELAUNCH=1") != std::wstring::npos);

    CHECK(ParseInstallKind("msi", InstallKind::Portable) == InstallKind::Msi);
    CHECK(ParseInstallKind("??", InstallKind::InnoUser) == InstallKind::InnoUser);
}
