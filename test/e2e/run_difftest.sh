#!/usr/bin/env bash
# Differential testing: run every e2e case through both fakecc and an oracle
# compiler (gcc on Linux, clang on macOS), compare exit codes.
set -uo pipefail
# Portability helpers (timeout/nproc/readelf shims for non-GNU hosts).
_COMPAT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Abort if the shims could not be sourced: a silently missing `timeout` turns
# every case into a confusing "command not found" instead of a clear error.
. "$_COMPAT_DIR/compat.sh" || { echo "cannot source compat.sh from $_COMPAT_DIR" >&2; exit 2; }
# run_e2e fans cases out to `xargs bash -c` workers, which only see exported
# functions, so re-export the shims this script's workers rely on.
export -f timeout nproc 2>/dev/null || true


ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
FAKECC=${1:-"$ROOT/build/fakecc"}
shift || true
MANIFEST=${MANIFEST:-"$ROOT/test/e2e/difftest_manifest.txt"}
SUITE_DIR="$ROOT/test/e2e/cases"
# Oracle compiler: gcc by default, clang on macOS (pass --oracle clang).
ORACLE="gcc"
EXTRA_FLAGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --oracle)
            ORACLE="$2"
            shift 2
            ;;
        *)
            EXTRA_FLAGS+=("$1")
            shift
            ;;
    esac
done
# Extra fakecc flags (e.g. -O0); the oracle stays unmodified.
FCC_FLAGS="${CC_FLAGS:-} ${EXTRA_FLAGS[*]:-}"

if [ ! -x "$FAKECC" ]; then
    echo "difftest: fakecc not executable: $FAKECC" >&2
    exit 2
fi
# Collect every .c case except the gdb/debug suite (which uses gdb
# breakpoints, not exit codes) and the expect_error cases (rejected
# by design, the oracle would accept them).  Also skip files that import a
# package other than `runtime`: the oracle has no package system, and stripping
# `import flags;` still leaves `flags.CONST` which it cannot parse.
files=()
for f in $(find "$SUITE_DIR" -name '*.c' -not -path '*/debug/*' | sort); do
    if grep -qE '^//[[:space:]]*expect_error' "$f"; then
        continue
    fi
    # Fakecc-runtime behaviour that glibc/libc does not match (locale-dependent
    # %lc/%ls, UTF-8 always, …).  e2e still runs these against expect:.
    if grep -qE '^//[[:space:]]*skip_difftest' "$f"; then
        continue
    fi
    if grep '^import[[:space:]]' "$f" | grep -vqE '^import[[:space:]]+runtime;'; then
        continue
    fi
    # When the oracle is clang, skip tests that use GCC-only builtins or
    # literal forms clang rejects (annotated with `skip_oracle: clang`).
    if [ "$ORACLE" = "clang" ]; then
        if grep -qE '__builtin_va_arg_pack' "$f"; then
            continue
        fi
        if grep -qE '^//[[:space:]]*skip_oracle:[[:space:]]*clang' "$f"; then
            continue
        fi
    fi
    files+=("$f")
done

if [ ${#files[@]} -eq 0 ]; then
    echo "difftest: no cases found in $SUITE_DIR" >&2
    exit 1
fi

exec bash "$ROOT/tools/difftest.sh" -c "$FAKECC" -f "$FCC_FLAGS" -o "$ORACLE" "${files[@]}"
