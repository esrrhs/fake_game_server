#!/usr/bin/env bash
# Porting loop for darwin-arm64.
#
# Each pass runs the unit tests, the main e2e suite, the multi-TU suite, the
# CLI suite, and re-audits the skipped cases.  The point is that none of them
# needs a human to decide whether the pass was good -- the numbers are
# comparable between passes, and a regression in any of them stops the loop.
#
# The multi-TU and CLI suites are here because the main e2e suite compiles one
# TU at a time and never reads an object it did not write, so a multi-TU or
# foreign-object regression is invisible to it.  The shared-library suite is
# reported only: it is ELF end to end and skips itself elsewhere.
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

# Set when a pass fails, so the trap can keep the logs.  They are the only
# record of what the suite actually said -- a "UNIT TESTS FAILED" with no log
# behind it is not diagnosable, and cleaning up on the way out used to throw
# them away exactly when they were needed.
PASS_FAILED=0

cleanup() {
    rc=$?
    rm -rf "$LOCKDIR" 2>/dev/null || true
    rm -f "$LOCK.owner" 2>/dev/null || true
    if [ "$rc" -ne 0 ] && [ -d "$RUN_DIR" ]; then
        mkdir -p "${RUN_DIR}.failed" 2>/dev/null || true
        mv "$RUN_DIR" "${RUN_DIR}.failed" 2>/dev/null || true
        echo "pass failed; logs kept in ${RUN_DIR}.failed" >&2
    else
        rm -rf "$RUN_DIR" 2>/dev/null || true
    fi
    exit $rc
}
trap cleanup EXIT

record_state() { printf '%s\n' "$1" > "$STATE" 2>/dev/null || true; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

PY=${PYTHON:-/Users/mingming/.workbuddy/binaries/python/versions/3.13.12/bin/python3}
FAKECC="$ROOT/build-local/fakecc"

# The suites are the reference point; a pass that does not match them is a
# regression regardless of how good the change looked.  These are per-host,
# because "skipped" and "not applicable" both count differently: the main e2e
# suite is ported in both directions, while run_multi_e2e and run_shlib_e2e
# carry assertions that only exist for ELF.
#
# They went stale once and made the loop permanently red -- a stale skip count
# fails the `skipped -ne baseline` arm on every pass, which is the same as no
# loop at all.  Re-measure them whenever a skip is removed, and prefer
# tools/skip_audit.py over hand-counting.
HOST_OS=$(uname -s | tr 'A-Z' 'a-z')
if [ "$HOST_OS" = "darwin" ]; then
    E2E_BASE_PASS=2682
    E2E_BASE_SKIP=7
    # The remaining multi-TU failures are readelf / R_X86_64 assertions with no
    # Mach-O counterpart, the suite asking gcc for x86-only options, and the
    # deliberate __StaticInit refusal.  A regression here means a real count.
    MULTI_BASE_PASS=84
    MULTI_BASE_FAIL=26
else
    E2E_BASE_PASS=2674
    E2E_BASE_SKIP=13
    MULTI_BASE_PASS=0
    MULTI_BASE_FAIL=999
fi
UNIT_TOTAL=26
# run_cli_e2e is host-independent: it drives the driver's argument handling.
CLI_BASE_FAIL=0

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

# A pass that starts while the tree is dirty is measuring a tree that is about
# to change underneath it: the build here rebuilds, and anything already
# running -- a suite started by hand, an editor's language server -- ends up
# half-way through against binaries that were replaced.  That reads as a
# failure in whichever stage lost the race, and cost a real investigation
# before it was recognised for what it was.  With a per-minute trigger this
# happens constantly, so it is checked rather than assumed.
tree_busy() {
    local dirty
    dirty=$(git -C "$ROOT" status --porcelain 2>/dev/null | head -1)
    if [ -n "$dirty" ]; then
        log "   tree has uncommitted changes; skipping this pass"
        log "   ($dirty)"
        log "   commit or stash first -- a pass here would race the rebuild"
        return 1
    fi
    # Someone else already inside the loop: its lock is the authority on that,
    # and we hold it, so the only remaining source is a second tree-level job.
    if pgrep -f "ctest|run_e2e.sh|run_multi_e2e.sh|run_cli_e2e.sh|run_shlib_e2e.sh" \
            >/dev/null 2>&1; then
        log "   another ctest or e2e run is in flight; skipping this pass"
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

# The main e2e suite compiles one TU at a time and never reads an object it
# did not write, so a multi-TU or foreign-object regression is invisible to it.
# Four such bugs lived there for a while (the tlv descriptor table keyed by
# name, PAGEOFF12 scaled differently in loads than in adds, __mod_init_func's
# eight-byte stride, and a SysV union rule that AAPCS64 does not have), so
# the suites that can see them belong in the loop.
multi_e2e() {
    log "== multi-TU e2e (about 2 minutes)"
    bash "$ROOT/test/e2e/run_multi_e2e.sh" "$RUN_DIR/fakecc" \
        >"$RUN_DIR/multi.log" 2>&1
    local n_pass n_fail
    n_pass=$(grep -c '^PASS' "$RUN_DIR/multi.log" || true)
    n_fail=$(grep -c '^FAIL' "$RUN_DIR/multi.log" || true)
    log "   $n_pass passed, $n_fail failed"
    if [ "$n_pass" -lt "$MULTI_BASE_PASS" ]; then
        log "   below baseline (expected >=$MULTI_BASE_PASS passed)"
        grep '^FAIL' "$RUN_DIR/multi.log" | head -20 | sed 's/^/     /'
        return 1
    fi
    if [ "$n_fail" -gt "$MULTI_BASE_FAIL" ]; then
        log "   above baseline (at most $MULTI_BASE_FAIL expected to fail)"
        grep '^FAIL' "$RUN_DIR/multi.log" | head -20 | sed 's/^/     /'
        return 1
    fi
    return 0
}

cli_e2e() {
    log "== CLI e2e"
    bash "$ROOT/test/e2e/run_cli_e2e.sh" "$RUN_DIR/fakecc" \
        >"$RUN_DIR/cli.log" 2>&1
    local line
    line=$(grep -oE '[0-9]+ passed, [0-9]+ failed' "$RUN_DIR/cli.log" | tail -1)
    log "   ${line:-no summary (see "$RUN_DIR/cli.log")}"
    if [ -z "$line" ]; then
        grep '^FAIL' "$RUN_DIR/cli.log" | head -20 | sed 's/^/     /'
        return 1
    fi
    if [ "$(echo "$line" | awk '{print $3}')" != "0" ]; then
        log "   FAILING CASES:"
        grep '^FAIL' "$RUN_DIR/cli.log" | head -20 | sed 's/^/     /'
        return 1
    fi
    return 0
}

# Reported, not gated: on darwin it skips itself as ELF-only, and on linux the
# baseline above is a placeholder, so there is nothing here to compare against.
shlib_e2e() {
    log "== shared-library e2e (ELF only; skips on non-linux)"
    bash "$ROOT/test/e2e/run_shlib_e2e.sh" "$RUN_DIR/fakecc" \
        >"$RUN_DIR/shlib.log" 2>&1
    local rc=$?
    log "   $(grep -E '^[0-9]+ passed|^SKIP' "$RUN_DIR/shlib.log" | head -1)"
    return 0
}

one_pass() {
    rule
    log "pass $*  -- $(date '+%H:%M:%S')"
    rule
    record_state "running $1 started=$(date '+%H:%M:%S') pid=$$"
    if ! tree_busy; then
        record_state "skipped-busy $(date '+%H:%M:%S')"
        rule
        log "pass $* skipped"
        # Not a failure: the next trigger gets a clean run.
        return 0
    fi
    if ! build; then
        record_state "build-failed $(date '+%H:%M:%S')"
        log "BUILD FAILED"
        PASS_FAILED=1
        return 2
    fi
    if ! unit_tests; then
        record_state "unit-failed $(date '+%H:%M:%S')"
        log "UNIT TESTS FAILED"
        PASS_FAILED=1
        return 3
    fi
    if ! e2e; then
        record_state "e2e-failed $(date '+%H:%M:%S')"
        log "E2E FAILED"
        PASS_FAILED=1
        return 4
    fi
    if ! multi_e2e; then
        record_state "multi-failed $(date '+%H:%M:%S')"
        log "MULTI-TU E2E FAILED"
        PASS_FAILED=1
        return 5
    fi
    if ! cli_e2e; then
        record_state "cli-failed $(date '+%H:%M:%S')"
        log "CLI E2E FAILED"
        PASS_FAILED=1
        return 6
    fi
    shlib_e2e
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
