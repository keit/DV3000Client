#pragma once

// Small disk cache for the raw response bodies of the live directories
// this app fetches at startup (reflectors, DMR talkgroups, DMR IDs) --
// each is re-fetched over the network every time regardless (data changes
// over time, and this is meant to be a "start fast, then catch up"
// cache, not a permanent offline store), but loading last run's copy
// synchronously at construction means the GUI has *something* useful to
// show immediately instead of an empty list/hardcoded fallback until the
// background fetch completes -- particularly valuable for the DMR ID
// directory, which is a multi-megabyte, ~330k-line fetch.
//
// Stored as one plain file per key under AppConfigLocation, right next to
// settings.json -- the raw response body verbatim (XML/JSON/semicolon-
// delimited text, whichever the source actually returns), not re-encoded,
// so each directory's own existing parser can be reused unchanged for
// both the live and cached path.

#include <QByteArray>
#include <QString>

namespace cache {

// The shared staleness threshold every directory in this app uses --
// matches Pi-Star's own DMR ID reload interval, and there's no reason for
// the reflector/talkgroup directories to churn on a different schedule.
constexpr qint64 ONE_DAY_SECONDS = 24 * 60 * 60;

// Reads the cached body for `key` (a short, filesystem-safe name, e.g.
// "dmr_ids"). Returns false if there's no cache yet or it's empty.
bool read(const QString &key, QByteArray &data);

// Writes `data` as the cached body for `key`, creating the cache
// directory if needed. Best-effort -- a failure here (disk full, no
// permission) just means the next startup re-fetches from scratch, same
// as if this cache never existed.
void write(const QString &key, const QByteArray &data);

// True if a cache entry exists for `key` and was written less than
// maxAgeSeconds ago -- callers use this to decide whether a live refresh
// is actually worth doing this run, rather than re-fetching on every
// single startup regardless of how current the cache already is. False
// (never "fresh") if there's no cache yet, so a fetch is always at least
// attempted once.
bool isFresh(const QString &key, qint64 maxAgeSeconds);

} // namespace cache
