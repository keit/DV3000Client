#pragma once

// Minimal raw-socket HTTP/1.0 GET, shared by every directory fetch in this
// project (xlx_directory.cpp originally had its own private copy; the new
// BrandMeister talkgroup directory needs the identical thing against a
// different host, so this is that logic pulled out to one place instead of
// duplicated). Deliberately not using libcurl/Qt Network: these are small,
// occasional, unauthenticated GETs against known plain-HTTP-capable APIs,
// not worth a new dependency for.

#include <string>

// GETs http://host:port/path with a 10s idle-read timeout. Returns the
// response body (headers stripped) via `body`, or false with `error` set
// on any resolve/connect/write/read/timeout/malformed-response failure.
bool httpGetRaw(const std::string &host, const std::string &path, int port, std::string &body, std::string &error);
