#pragma once
#include <string>
#include <string_view>

namespace ps {

// Quote one argument for the device's POSIX shell (mksh/toybox sh).
// Always single-quotes; embedded ' becomes '\''. Safe for any byte sequence
// except NUL (which can't appear in a path or argv anyway).
std::string ShellQuote(std::string_view arg);

// Quote one argument for a Windows command line so that CommandLineToArgvW
// (and the MSVC CRT, which adb.exe uses) reproduces it exactly.
std::wstring QuoteWindowsArg(std::wstring_view arg);

// Builder for an `adb shell` command line. Every arg() is quoted; anything
// that must stay unquoted (pipes, redirects, ;) goes through raw(), which
// keeps the unsafe spots easy to find in review.
//
//   ShellCommand("stat").arg("-c").arg("%s %Y %n").arg(path)
//   ShellCommand("find").arg(root).arg("-type").arg("f").raw("2>/dev/null")
class ShellCommand {
public:
    explicit ShellCommand(std::string_view program);

    ShellCommand& arg(std::string_view a);
    ShellCommand& raw(std::string_view fragment);
    // Append another full command, e.g. `a; b` or `a | b`.
    ShellCommand& then(std::string_view separator, const ShellCommand& next);

    const std::string& str() const { return m_cmd; }

private:
    std::string m_cmd;
};

} // namespace ps
