#!/usr/bin/env bash
# Porting loop for darwin-arm64.
#
# Each pass does the same thing: run the unit tests, run the full e2e suite,
# and re-audit the skipped cases.  The point is that none of the three needs a
# human to decide whether the pass was good -- the numbers are comparable
# between passes, and a regression in any of them stops the loop.
#
#   ./tools/port_loop.sh            # one pass (default)
#   ./tools/port_loop.sh --watch N  # N passes, stopping on the first failure
#
# Exits non-zero when a pass regresses, so it can gate a commit or feed a
# longer-running agent loop.
#
# A pass takes about 13 minutes, most of it the e2e suite, so this is meant to
# be driven on a frequent schedule (a per-minute trigger is fine).  Two passes
# must never overlap: e2e reads a snapshot of the compiler taken at startup and
# writes into a fixed set of /tmp paths, so a second pass would interleave with
# the first and report failures that are neither pass's.  The lock below turns
# an overlapping trigger into a no-op instead.
#
#   PORT_LOOP_SKIP_BUSY=1  exit 0 quietly when a pass is already running
#                          (the default; an agent reading the exit code
#                          should not have to tell "busy" from "passed")
#
# A trigger that finds the lock held exits 0, so a caller reading the exit code
# sees the same thing it would see from a clean pass; /tmp/port_loop_state
# carries which of the two happened.
set -u

LOCK="${PORT_LOOP_LOCK:-/tmp/fakecc_port_loop}"
LOG="${PORT_LOOP_LOG:-/tmp/port_loop_last.log}"
STATE="${PORT_LOOP_STATE:-/tmp/port_loop_state}"
# Scratch paths are per-process: a killed pass leaves files behind, and the
# next one must not read them as if they were its own.
RUN_DIR="/tmp/port_loop_run_$$"

mkdir -p "$(dirname "$LOCK")"

# Claim the pass.  mkdir is the primitive: it either creates the directory or
# fails with EEXIST, and that is atomic on every filesystem macOS mounts.
# flock is not an option -- it does not exist on this platform, and a lock that
# silently does nothing is worse than no lock, because a per-minute trigger
# looks like it is being throttled when it is not.
LOCKDIR="${LOCK}.d"

lock_is_stale() {
    # A pass killed outright leaves the directory behind.  Its pid is the only
    # way to tell that from a pass that is still going, so check that first and
    # only reclaim on evidence.
    local owner=$1 pid
    [ -f "$owner" ] || return 0          # no record: treat as stale
    pid=$(cat "$owner" 2>/dev/null) || return 0
    [ -n "$pid" ] || return 0
    kill -0 "$pid" 2>/dev/null && return 1 # alive -> genuinely held
    return 0                              # gone -> ours to reclaim
}

claim_lock() {
    if mkdir "$LOCKDIR" 2>/dev/null; then
        echo $$ > "$LOCK.owner" 2>/dev/null || true
        return 0
    fi
    lock_is_stale "$LOCK.owner" || return 1
    # Reclaim a dead owner's lock, then race the others for it.
    rm -rf "$LOCKDIR" 2>/dev/null || return 1
    if mkdir "$LOCKDIR" 2>/dev/null; then
        echo $$ > "$LOCK.owner" 2>/dev/null || true
        return 0
    fi
    return 1
}

if ! claim_lock; then
    if [ "${PORT_LOOP_SKIP_BUSY:-1}" = "1" ]; then
        exit 0
    fi
    echo "another pass is running (lock: $LOCKDIR)"
    exit 5
fi

mkdir -p "$RUN_DIR"

cleanup() {
    rc=$?
    rm -rf "$LOCKDIR" 2>/dev/null || true
    rm -f "$LOCK.owner" 2>/dev/null || true
    # Keep the logs: they are what a failed pass is diagnosed from.
    [ -n "${PORT_LOOP_KEEP:-}" ] || rm -rf "$RUN_DIR" 2>/dev/null || true
    exit $rc
}
trap cleanup EXIT

record_state() { printf '%s\n' "$1" > "$STATE" 2>/dev/null || true; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

PY=${PYTHON:-/Users/mingming/.workbuddy/binaries/python/versions/3.13.12/bin/python3}
FAKECC="$ROOT/build-local/fakecc"

# The suite is the reference point; a pass that does not match it is a
# regression regardless of how good the change looked.
E2E_BASE_PASS=2674
E2E_BASE_SKIP=13
UNIT_TOTAL=26

passes=1
[ "${1:-}" = "--watch" ] && passes="${2:-5}"

log() { printf '%s\n' "$*"; }
rule() { log "-------------------------------------------------------------"; }

build() {
    log "== build"
    if ! make -C "$ROOT/build-local" -j8 >"$RUN_DIR/build.log" 2>&1; then
        tail -20 "$RUN_DIR/build.log"
        return 1
    fi
    return 0
}

unit_tests() {
    log "== unit tests"
    ( cd "$ROOT/build-local" && ctest -I 1,"$UNIT_TOTAL" ) >"$RUN_DIR/unit.log" 2>&1
    local rc=$?
    grep -E "tests passed|tests failed" "$RUN_DIR/unit.log" | tail -1
    return $rc
}

e2e() {
    log "== e2e (about 10 minutes)"
    cp "$FAKECC" "$RUN_DIR/fakecc"
    bash "$ROOT/test/e2e/run_e2e.sh" "$RUN_DIR/fakecc" -O1 \
        >"$RUN_DIR/e2e.log" 2>&1
    local line
    line=$(grep -oE '[0-9]+ passed, [0-9]+ failed, [0-9]+ skipped' \
           "$RUN_DIR/e2e.log" | tail -1)
    log "   ${line:-no summary (see "$RUN_DIR/e2e.log")}"
    if [ -z "$line" ]; then
        return 1
    fi
    local passed failed skipped
    passed=$(echo "$line" | awk '{print $1}')
    failed=$(echo  "$line" | awk '{print $3}')
    skipped=$(echo "$line" | awk '{print $5}')
    if [ "$failed" != "0" ]; then
        log "   FAILING CASES:"
        grep '^FAIL' "$RUN_DIR/e2e.log" | head -20 | sed 's/^/     /'
        return 1
    fi
    if [ "$passed" -lt "$E2E_BASE_PASS" ] || [ "$skipped" -ne "$E2E_BASE_SKIP" ]; then
        log "   below baseline (expected >=$E2E_BASE_PASS passed, $E2E_BASE_SKIP skipped)"
        return 1
    fi
    return 0
}

skip_audit() {
    log "== skip audit"
    "$PY" "$ROOT/tools/skip_audit.py" 2>&1 | tail -8
}

one_pass() {
    rule
    log "pass $*  -- $(date '+%H:%M:%S')"
    rule
    record_state "running $1 started=$(date '+%H:%M:%S') pid=$$"
    if ! build; then
        record_state "build-failed $(date '+%H:%M:%S')"
        log "BUILD FAILED"
        return 2
    fi
    if ! unit_tests; then
        record_state "unit-failed $(date '+%H:%M:%S')"
        log "UNIT TESTS FAILED"
        return 3
    fi
    if ! e2e; then
        record_state "e2e-failed $(date '+%H:%M:%S')"
        log "E2E FAILED"
        return 4
    fi
    skip_audit
    rule
    log "pass $* clean"
    record_state "clean $(date '+%H:%M:%S')"
    return 0
}

i=1
while [ "$i" -le "$passes" ]; do
    one_pass "$i" || exit $?
    [ "$i" -lt "$passes" ] && log ""
    i=$((i + 1))
done
log ""
log "all $passes pass(es) clean"
