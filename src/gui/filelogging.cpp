#include "filelogging.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include <cerrno>
#include <unistd.h>

#include <QDir>
#include <QStandardPaths>

namespace {

// "YYYY-MM-DD HH:MM:SS.mmm" -- millisecond precision matters here since
// several of the messages this ends up prefixing (AMBE decode retries,
// ALSA xruns) can fire many times within the same second.
std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t nowTimeT = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm localNow;
    ::localtime_r(&nowTimeT, &localNow);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localNow);

    char withMs[40];
    std::snprintf(withMs, sizeof(withMs), "%s.%03lld", buf, static_cast<long long>(ms.count()));
    return withMs;
}

} // namespace

QString logFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + "/dv3000client.log";
}

QString startFileLogging() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    QString logPath = logFilePath();

    // Open (and fail loudly if that doesn't work) before touching stderr
    // at all -- once fd 2 is redirected below, a plain fprintf(stderr,...)
    // here would just vanish into the pipe with nothing reading it yet.
    std::string logPathStd = logPath.toStdString();
    FILE *logFile = std::fopen(logPathStd.c_str(), "w");
    if (!logFile) {
        std::fprintf(stderr, "dv3kclient: could not open log file %s: %s\n",
                     logPathStd.c_str(), std::strerror(errno));
        return {};
    }

    int pipeFds[2];
    if (::pipe(pipeFds) != 0) {
        std::fclose(logFile);
        return {};
    }

    // Keep a separate fd pointing at wherever stderr originally went (a
    // real terminal, or nothing/closed if launched without one) so the
    // background thread below can still forward there after fd 2 itself
    // gets repointed at the pipe.
    int originalStderr = ::dup(STDERR_FILENO);
    if (originalStderr < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        std::fclose(logFile);
        return {};
    }

    if (::dup2(pipeFds[1], STDERR_FILENO) < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::close(originalStderr);
        std::fclose(logFile);
        return {};
    }
    ::close(pipeFds[1]); // fd 2 is now the write end; this extra copy of it isn't needed

    // Every fprintf(stderr, ...) anywhere in the process now lands in the
    // pipe. This thread just relays each chunk to both destinations for as
    // long as the process runs -- it naturally exits via read() returning
    // 0 once fd 2 (the pipe's only remaining write end) closes at process
    // exit, so it's deliberately left detached rather than joined anywhere.
    //
    // The terminal side gets each raw chunk verbatim, unchanged from
    // before -- live output keeps looking exactly like it always has. The
    // file side is timestamped per *line* instead, which needs buffering:
    // a read() chunk boundary has no relation to line boundaries (one
    // fprintf's output can span multiple reads, or one read can contain
    // several lines), so lines are only known to be complete once a '\n'
    // shows up in the accumulated buffer.
    std::thread([readFd = pipeFds[0], originalStderr, logFile] {
        std::string lineBuf;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(readFd, buf, sizeof(buf))) > 0) {
            // Best-effort: there may be no real terminal on the other end
            // (e.g. launched from a desktop icon) -- nothing to do about
            // that beyond letting the file logging below carry on regardless.
            ssize_t written = ::write(originalStderr, buf, static_cast<size_t>(n));
            (void)written;

            lineBuf.append(buf, static_cast<size_t>(n));
            size_t newlinePos;
            while ((newlinePos = lineBuf.find('\n')) != std::string::npos) {
                std::fprintf(logFile, "[%s] %s\n", currentTimestamp().c_str(), lineBuf.substr(0, newlinePos).c_str());
                lineBuf.erase(0, newlinePos + 1);
            }
            std::fflush(logFile); // survive a crash, not just a clean exit
        }
        // Whatever's left never got a trailing newline (e.g. the process
        // died mid-fprintf) -- log it anyway rather than silently dropping it.
        if (!lineBuf.empty()) std::fprintf(logFile, "[%s] %s\n", currentTimestamp().c_str(), lineBuf.c_str());
        std::fclose(logFile);
        ::close(readFd);
        ::close(originalStderr);
    }).detach();

    return logPath;
}
