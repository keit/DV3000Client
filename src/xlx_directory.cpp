#include "xlx_directory.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

namespace xlx {

namespace {

constexpr const char *XLX_API_HOST = "xlxapi.rlx.lu";
constexpr const char *XLX_API_PATH = "/api.php?do=GetReflectorList";
constexpr int HTTP_PORT = 80;
constexpr int HTTP_TIMEOUT_SEC = 10;

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Raw-socket HTTP/1.0 GET against xlxapi.rlx.lu, mirroring the pattern
// already vendored in third_party/xlxd/src/cysfnodedirhttp.cpp for the same
// server, but resolving with getaddrinfo (matching this app's own
// convention in dextra_test.cpp) and properly splitting off the HTTP
// headers before returning the body.
bool httpGetRaw(const std::string &host, const std::string &path, int port,
                 std::string &body, std::string &error) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        error = "cannot resolve " + host;
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket() failed";
        freeaddrinfo(res);
        return false;
    }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        error = "connect() to " + host + " failed";
        freeaddrinfo(res);
        ::close(fd);
        return false;
    }
    freeaddrinfo(res);

    std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                           "\r\nUser-Agent: dv3000client\r\nConnection: close\r\n\r\n";
    if (::write(fd, request.data(), request.size()) < 0) {
        error = "write() to " + host + " failed";
        ::close(fd);
        return false;
    }

    std::string response;
    char buf[4096];
    for (;;) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        struct timeval timeout{HTTP_TIMEOUT_SEC, 0};
        int sel = ::select(fd + 1, &readSet, nullptr, nullptr, &timeout);
        if (sel <= 0) {
            error = "timed out waiting for " + host;
            ::close(fd);
            return false;
        }
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        error = "malformed HTTP response from " + host;
        return false;
    }
    body = response.substr(headerEnd + 4);
    return true;
}

// Naive substring tag search -- no escaping/CDATA handling, but sufficient
// for the XLX API's fixed, well-known schema (mirrors the same algorithm
// the PHP dashboard uses in its own class.parsexml.php).
std::string getElement(const std::string &block, const std::string &tag) {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t start = block.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    size_t end = block.find(close, start);
    if (end == std::string::npos) return "";
    return trim(block.substr(start, end - start));
}

std::vector<ReflectorInfo> parseReflectorListXml(const std::string &xml) {
    std::vector<ReflectorInfo> result;
    const std::string openTag = "<reflector>";
    const std::string closeTag = "</reflector>";
    size_t pos = 0;
    while (true) {
        size_t start = xml.find(openTag, pos);
        if (start == std::string::npos) break;
        start += openTag.size();
        size_t end = xml.find(closeTag, start);
        if (end == std::string::npos) break;
        std::string block = xml.substr(start, end - start);
        pos = end + closeTag.size();

        ReflectorInfo info;
        info.name = getElement(block, "name");
        info.host = getElement(block, "lastip");
        info.country = getElement(block, "country");
        info.comment = getElement(block, "comment");
        std::string lastContact = getElement(block, "lastcontact");
        info.lastContactUnix = lastContact.empty() ? 0 : std::atol(lastContact.c_str());

        if (!info.name.empty() && !info.host.empty()) {
            result.push_back(std::move(info));
        }
    }
    return result;
}

// Parses the trailing run of digits out of a reflector name ("XLX123" ->
// 123) or a bare numeric query ("123" -> 123). Returns -1 if `s` has no
// digits at all (e.g. the special-named "XRFWDX").
int extractNumber(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) i++;
    if (i == s.size()) return -1;
    return std::atoi(s.c_str() + i);
}

} // namespace

bool fetchReflectorList(std::vector<ReflectorInfo> &out, std::string &error) {
    std::string body;
    if (!httpGetRaw(XLX_API_HOST, XLX_API_PATH, HTTP_PORT, body, error)) {
        return false;
    }
    out = parseReflectorListXml(body);
    if (out.empty()) {
        error = "no reflectors parsed from " + std::string(XLX_API_HOST) + " response";
        return false;
    }
    return true;
}

bool loadStaticFallback(const std::string &path, std::vector<ReflectorInfo> &out) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "xlx_directory: cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::istringstream iss(t);
        ReflectorInfo info;
        if (!(iss >> info.name >> info.host)) continue;
        out.push_back(std::move(info));
    }
    return true;
}

const ReflectorInfo *findReflector(const std::vector<ReflectorInfo> &live,
                                    const std::vector<ReflectorInfo> &fallback,
                                    const std::string &query) {
    int wanted = extractNumber(query);
    if (wanted < 0) return nullptr;

    for (const auto &r : live) {
        if (extractNumber(r.name) == wanted) return &r;
    }
    for (const auto &r : fallback) {
        if (extractNumber(r.name) == wanted) return &r;
    }
    return nullptr;
}

} // namespace xlx
