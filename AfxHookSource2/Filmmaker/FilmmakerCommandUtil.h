#pragma once

// Small helper shared by the mirv_filmmaker command files that print base64-chunked state
// (netcon truncates a single console line, so "...state64" commands split the encoded JSON
// into ~120-char chunks the caller re-assembles). Split out of FilmmakerCommand.cpp because
// both FollowCommand.cpp ("follow state64") and CameraEditorCommand.cpp ("editor state64")
// need it.

#include <string>

namespace Filmmaker {

void PrintEncodedState(const char* marker, const std::string& json);

} // namespace Filmmaker
