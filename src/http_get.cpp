#include "http_get.h"

#include <cstdio>

#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

namespace {
constexpr int HTTP_TIMEOUT_SEC = 10;
} // namespace

bool httpGetRaw(const std::string &host, const std::string &path, int port, std::string &body, std::string &error) {
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
