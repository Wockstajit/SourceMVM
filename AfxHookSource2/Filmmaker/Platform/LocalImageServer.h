#pragma once

// Pictures for Panorama that do not exist as compiled game resources. Panorama cannot show a loose
// PNG (file:// wants a compiled .vtex_c) but it does load http images, so a tiny HTTP server bound to
// 127.0.0.1 on a random port, under a random per-session path, serves them. Only what a registered
// source hands out is reachable; nothing else on disk is.
//
// Users: the osu easter egg's skin images (EasterEggs/Osu/OsuSkin.cpp) and the alternative
// environment's top-down map pictures (AltEnvironment/AltEnvironment.cpp).

#include <functional>
#include <string>

namespace Filmmaker::LocalImageServer {

// Fills `body` and `contentType` for `name` (the part of the path after the prefix, query string
// removed). Runs on the server thread. False answers 404.
using Source = std::function<bool(const std::string &name, std::string &body, std::string &contentType)>;

// Requests for /<token>/<prefix>/<name> go to `source`. Starts the server on first use; registering a
// prefix again replaces its source.
void Register(const std::string &prefix, Source source);

// "http://127.0.0.1:<port>/<token>/<prefix>/", or "" when the server could not start.
std::string Url(const std::string &prefix);

int Port();
unsigned Served();

} // namespace Filmmaker::LocalImageServer
