#include "filelogging.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <cerrno>
#include <unistd.h>

#include <QDir>
#include <QStandardPaths>

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
        std::fprintf(stderr, "dv3000client_gui: could not open log file %s: %s\n",
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
    std::thread([readFd = pipeFds[0], originalStderr, logFile] {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(readFd, buf, sizeof(buf))) > 0) {
            // Best-effort: there may be no real terminal on the other end
            // (e.g. launched from a desktop icon) -- nothing to do about
            // that beyond letting the file logging below carry on regardless.
            ssize_t written = ::write(originalStderr, buf, static_cast<size_t>(n));
            (void)written;
            if (logFile) {
                std::fwrite(buf, 1, static_cast<size_t>(n), logFile);
                std::fflush(logFile); // survive a crash, not just a clean exit
            }
        }
        if (logFile) std::fclose(logFile);
        ::close(readFd);
        ::close(originalStderr);
    }).detach();

    return logPath;
}
