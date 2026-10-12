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
set -u

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
    if ! make -C "$ROOT/build-local" -j8 >/tmp/port_loop_build.log 2>&1; then
        tail -20 /tmp/port_loop_build.log
        return 1
    fi
    return 0
}

unit_tests() {
    log "== unit tests"
    ( cd "$ROOT/build-local" && ctest -I 1,"$UNIT_TOTAL" ) >/tmp/port_loop_unit.log 2>&1
    local rc=$?
    grep -E "tests passed|tests failed" /tmp/port_loop_unit.log | tail -1
    return $rc
}

e2e() {
    log "== e2e (about 10 minutes)"
    cp "$FAKECC" /tmp/port_loop_fakecc
    bash "$ROOT/test/e2e/run_e2e.sh" /tmp/port_loop_fakecc -O1 \
        >/tmp/port_loop_e2e.log 2>&1
    local line
    line=$(grep -oE '[0-9]+ passed, [0-9]+ failed, [0-9]+ skipped' \
           /tmp/port_loop_e2e.log | tail -1)
    log "   ${line:-no summary (see /tmp/port_loop_e2e.log)}"
    if [ -z "$line" ]; then
        return 1
    fi
    local passed failed skipped
    passed=$(echo "$line" | awk '{print $1}')
    failed=$(echo  "$line" | awk '{print $3}')
    skipped=$(echo "$line" | awk '{print $5}')
    if [ "$failed" != "0" ]; then
        log "   FAILING CASES:"
        grep '^FAIL' /tmp/port_loop_e2e.log | head -20 | sed 's/^/     /'
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
    build || { log "BUILD FAILED"; return 2; }
    unit_tests || { log "UNIT TESTS FAILED"; return 3; }
    e2e || { log "E2E FAILED"; return 4; }
    skip_audit
    rule
    log "pass $* clean"
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
