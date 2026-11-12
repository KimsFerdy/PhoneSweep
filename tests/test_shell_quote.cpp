#include "adb/ShellCommand.h"
#include "util/Utf8.h"

#include <doctest/doctest.h>

#include <windows.h>
#include <shellapi.h>

#include <optional>
#include <string>
#include <vector>

using namespace ps;

namespace {

// Minimal POSIX sh word splitter covering the quoting forms ShellQuote emits
// ('...' and \x outside quotes) plus double quotes, so we can round-trip.
std::optional<std::vector<std::string>> ShSplit(std::string_view s)
{
    std::vector<std::string> words;
    std::string cur;
    bool inWord = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'') {
            const size_t end = s.find('\'', i + 1);
            if (end == std::string_view::npos)
                return std::nullopt; // unterminated
            cur.append(s.substr(i + 1, end - i - 1));
            i = end;
            inWord = true;
        } else if (c == '\\') {
            if (i + 1 >= s.size())
                return std::nullopt;
            cur.push_back(s[++i]);
            inWord = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (inWord) {
                words.push_back(cur);
                cur.clear();
                inWord = false;
            }
        } else if (c == '$' || c == '`' || c == '"' || c == ';' || c == '|' || c == '&' || c == '*' || c == '?'
            || c == '(' || c == ')' || c == '<' || c == '>') {
            return std::nullopt; // special char escaped quoting: would be interpreted by sh
        } else {
            cur.push_back(c);
            inWord = true;
        }
    }
    if (inWord)
        words.push_back(cur);
    return words;
}

std::vector<std::wstring> WinSplit(const std::wstring& cmdLine)
{
    int argc = 0;
    // Prefix a dummy program name: the first token is parsed with different rules.
    const std::wstring full = L"prog.exe " + cmdLine;
    LPWSTR* argv = CommandLineToArgvW(full.c_str(), &argc);
    std::vector<std::wstring> out(argv + 1, argv + argc);
    LocalFree(argv);
    return out;
}

const char* kNastyArgs[] = {
    "",
    "plain",
    "with space",
    "it's",
    "'",
    "''",
    "a'b'c",
    "$HOME",
    "`reboot`",
    "$(rm -rf /)",
    "semi;colon",
    "pipe|amp&",
    "star*?",
    "back\\slash",
    "trailing\\",
    "double\"quote",
    "new\nline",
    "tab\there",
    "/sdcard/WhatsApp/Media/WhatsApp Images/IMG-2024 (1).jpg",
    "/sdcard/Musique/Fally Ipupa – Éloko Oyo.mp3",       // French accents + en dash
    "/sdcard/Download/Kiswahili ya leo 😀.pdf",          // emoji
    "/sdcard/DCIM/Mbóka ya Lingala ɛ ɔ/photo.jpg",        // Lingala open vowels
    "/sdcard/日本語/文件.txt",
    "-rf",
    "--",
};

} // namespace

TEST_CASE("ShellQuote wraps in single quotes")
{
    CHECK(ShellQuote("abc") == "'abc'");
    CHECK(ShellQuote("") == "''");
    CHECK(ShellQuote("a b") == "'a b'");
    CHECK(ShellQuote("it's") == "'it'\\''s'");
    CHECK(ShellQuote("$HOME") == "'$HOME'");
}

TEST_CASE("ShellQuote round-trips through sh word splitting")
{
    for (const char* arg : kNastyArgs) {
        CAPTURE(arg);
        const auto words = ShSplit(ShellQuote(arg));
        REQUIRE(words.has_value());
        REQUIRE(words->size() == 1);
        CHECK((*words)[0] == arg);
    }
}

TEST_CASE("ShellCommand builds a multi-arg line that splits back exactly")
{
    ShellCommand cmd("stat");
    cmd.arg("-c").arg("%s %Y %n").arg("/sdcard/it's here/😀.jpg").arg("$(reboot)");
    const auto words = ShSplit(cmd.str());
    REQUIRE(words.has_value());
    REQUIRE(words->size() == 5);
    CHECK((*words)[0] == "stat");
    CHECK((*words)[1] == "-c");
    CHECK((*words)[2] == "%s %Y %n");
    CHECK((*words)[3] == "/sdcard/it's here/😀.jpg");
    CHECK((*words)[4] == "$(reboot)");
}

TEST_CASE("ShellCommand raw() and then() are passed verbatim")
{
    ShellCommand a("find");
    a.arg("/storage/emulated/0/").raw("2>/dev/null");
    ShellCommand b("wc");
    b.arg("-l");
    a.then("|", b);
    CHECK(a.str() == "find '/storage/emulated/0/' 2>/dev/null | wc '-l'");
}

TEST_CASE("QuoteWindowsArg round-trips through CommandLineToArgvW")
{
    for (const char* arg : kNastyArgs) {
        CAPTURE(arg);
        const std::wstring w = Widen(arg);
        const auto split = WinSplit(QuoteWindowsArg(w));
        REQUIRE(split.size() == 1);
        CHECK(Narrow(split[0]) == arg);
    }
}

TEST_CASE("QuoteWindowsArg: several args in one command line")
{
    std::wstring line;
    for (const char* arg : kNastyArgs) {
        if (!line.empty())
            line.push_back(L' ');
        line += QuoteWindowsArg(Widen(arg));
    }
    const auto split = WinSplit(line);
    REQUIRE(split.size() == std::size(kNastyArgs));
    for (size_t i = 0; i < split.size(); ++i)
        CHECK(Narrow(split[i]) == kNastyArgs[i]);
}

TEST_CASE("QuoteWindowsArg leaves simple args untouched")
{
    CHECK(QuoteWindowsArg(L"devices") == L"devices");
    CHECK(QuoteWindowsArg(L"-l") == L"-l");
    CHECK(QuoteWindowsArg(L"") == L"\"\"");
    CHECK(QuoteWindowsArg(L"C:\\Program Files\\adb.exe") == L"\"C:\\Program Files\\adb.exe\"");
}
