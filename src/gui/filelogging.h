#pragma once

// Tees the process's stderr into a log file, in addition to (not instead
// of) wherever it was already going -- a real terminal keeps showing live
// output exactly as before, but everything is also captured to disk. This
// covers every existing fprintf(stderr, ...) call throughout
// dextra_client/dextra_audio and the vendored serialDV library (ThumbDV
// chip communication, ALSA xrun/error messages, connection/header
// logging, ...) without needing to touch any of those call sites --
// stderr is a single OS-level file descriptor shared by all of them.

#include <QString>

// Where the log file lives -- same formula startFileLogging() itself uses,
// factored out so callers that just want to display/open the path (e.g. a
// "Log File Location..." menu action) don't have to duplicate it.
QString logFilePath();

// Call once, as early as possible in main() -- before QApplication, so
// even startup issues get captured. Returns the log file's path (the same
// value logFilePath() would give), or an empty string if setup failed, in
// which case stderr is left untouched.
QString startFileLogging();
