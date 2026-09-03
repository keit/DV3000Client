#!/bin/sh
# Build and run a local xlxd reflector for testing dextra_test against,
# reproducing the setup used to verify the DExtra client end to end (see
# the "Implement minimal DExtra client" and "Route DExtra voice frames
# through a real ThumbDV vocoder" commits).
#
# xlxd (third_party/xlxd) is vendored unmodified. Two local-testing-only
# toggles -- run in the foreground instead of daemonizing, and skip the
# G3 protocol's raw ICMP socket (needs root, unrelated to DExtra) -- are
# kept as scripts/xlxd-local-test.patch and applied here at build time
# rather than committed into the vendored source.
#
# Usage: scripts/run-test-xlxd.sh [callsign] [listen ip]
#   Defaults to a local-only reflector: XLX999 on 127.0.0.1, DExtra port
#   30001. dextra_test can then link with: dextra_test 127.0.0.1 <module> ...

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/xlxd/src"
CALLSIGN="${1:-XLX999}"
LISTEN_IP="${2:-127.0.0.1}"

cd "$ROOT/third_party/xlxd"
if ! git apply --check "$ROOT/scripts/xlxd-local-test.patch" 2>/dev/null; then
    if ! git apply --reverse --check "$ROOT/scripts/xlxd-local-test.patch" 2>/dev/null; then
        echo "run-test-xlxd: patch doesn't apply cleanly against the vendored xlxd -- check third_party/xlxd is unmodified and at its pinned commit" >&2
        exit 1
    fi
    echo "run-test-xlxd: local-test patch already applied"
else
    git apply "$ROOT/scripts/xlxd-local-test.patch"
    echo "run-test-xlxd: applied local-test patch"
fi

make -C "$SRC" -j"$(nproc)"

echo "run-test-xlxd: starting $CALLSIGN on $LISTEN_IP (DExtra port 30001)"
exec "$SRC/xlxd" "$CALLSIGN" "$LISTEN_IP" "$LISTEN_IP"
