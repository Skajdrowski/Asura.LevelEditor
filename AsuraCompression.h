#pragma once

#include "asura_base.hpp"

namespace asura {

// Maps an ordinary Asura file, or maps and transparently expands an AsuraCmp
// file. The returned storage is released with unmap_file in either case.
bool map_asura_file(const char* path, MappedFile* out, Error* err);
bool map_asura_file(const wchar_t* path, MappedFile* out, Error* err);

} // namespace asura
