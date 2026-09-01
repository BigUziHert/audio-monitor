#pragma once
//
// UTF-8 <-> UTF-16 conversion. Windows APIs are wide; everything we log or
// persist is UTF-8.
//
#include <string>

namespace audiomon {

std::string  toUtf8(const std::wstring& w);
std::wstring toWide(const std::string& s);

} // namespace audiomon
