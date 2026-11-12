#pragma once
#include <string>
#include <string_view>

namespace ps {

// UTF-8 <-> UTF-16 conversion. Everything inside the app is UTF-8; we only
// widen at the Win32 boundary.
std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view utf16);

} // namespace ps
