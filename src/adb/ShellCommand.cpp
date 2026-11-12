#include "adb/ShellCommand.h"

namespace ps {

std::string ShellQuote(std::string_view arg)
{
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('\'');
    for (char c : arg) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::wstring QuoteWindowsArg(std::wstring_view arg)
{
    // Rules from "Everyone quotes command line arguments the wrong way" (MSDN blog):
    // backslashes are literal unless they precede a double quote.
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(arg);

    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            // Double trailing backslashes so the closing quote isn't escaped.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

ShellCommand::ShellCommand(std::string_view program)
    : m_cmd(program)
{
}

ShellCommand& ShellCommand::arg(std::string_view a)
{
    m_cmd.push_back(' ');
    m_cmd += ShellQuote(a);
    return *this;
}

ShellCommand& ShellCommand::raw(std::string_view fragment)
{
    m_cmd.push_back(' ');
    m_cmd += fragment;
    return *this;
}

ShellCommand& ShellCommand::then(std::string_view separator, const ShellCommand& next)
{
    m_cmd.push_back(' ');
    m_cmd += separator;
    m_cmd.push_back(' ');
    m_cmd += next.str();
    return *this;
}

} // namespace ps
