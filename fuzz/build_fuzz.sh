#!/usr/bin/env bash
# Build the libFuzzer targets: clang builds libfakecc.a with sanitizers and
# coverage instrumentation (fuzzer-no-link), then each target is linked against
# it with libFuzzer's own main.
#
#   fuzz/build_fuzz.sh          # build
#   fuzz/build_fuzz.sh test     # build, then 1000 runs of each target
#   fuzz/build_fuzz.sh clean    # remove build_fuzz/
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BUILD_DIR:-"$ROOT/build_fuzz"}
CLANG=${CLANG:-clang}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
CORPUS_MAX=${CORPUS_MAX:-300}   # seed files taken from each test suite

if [ "${1:-}" = clean ]; then
    rm -rf "$BUILD"
    exit 0
fi

command -v "$CLANG" >/dev/null || { echo "build_fuzz: clang not found (set CLANG=)"; exit 2; }

# 1. libfakecc.a: ASan + UBSan for the bugs, -fuzzer-no-link for the coverage
#    feedback libFuzzer steers with.  -O1 keeps the fuzzer fast enough.
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_C_COMPILER="$CLANG" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined,fuzzer-no-link -fno-omit-frame-pointer -g -O1" \
    >/dev/null
cmake --build "$BUILD" --parallel "$JOBS" --target fakecc_core >/dev/null

# 2. Seed corpus: the existing suites are far better starting points than random
#    bytes.  The differential target can only use plain C — the e2e cases are
#    written in fakecc's own dialect, which gcc cannot parse.
seed_corpus() {
    local dir=$1 src=$2 n f
    mkdir -p "$dir"
    n=$(find "$dir" -name '*.c' | wc -l)
    while IFS= read -r f; do
        cp "$f" "$dir/$(printf '%04d' $n)_$(basename "$f")"
        n=$((n + 1))
    done < <(find "$src" -name '*.c' -size -8k | sort | awk 'NR % 8 == 1' | head -"$CORPUS_MAX")
    echo "  $dir: $n files"
}
echo "seeding corpus from the existing test suites"
seed_corpus "$BUILD/corpus/compile" "$ROOT/test/e2e/cases"
seed_corpus "$BUILD/corpus/compile" "$ROOT/test/compile/gcc_compile"
seed_corpus "$BUILD/corpus/differential" "$ROOT/test/compile/gcc_compile"

# 3. The targets themselves
mkdir -p "$BUILD/bin"
for t in fuzz_compile fuzz_differential; do
    "$CLANG" -std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g \
        -I"$ROOT/include" -I"$ROOT/src" \
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        "$ROOT/fuzz/$t.c" "$BUILD/libfakecc.a" -o "$BUILD/bin/$t"
    echo "built $BUILD/bin/$t"
done

if [ "${1:-}" = test ]; then
    for t in fuzz_compile fuzz_differential; do
        echo "--- $t: 1000 runs ---"
        (cd "$BUILD" && ASAN_OPTIONS=detect_leaks=0 "./bin/$t" \
            "corpus/${t#fuzz_}" -dict="$ROOT/fuzz/c_keywords.dict" \
            -max_len=2048 -runs=1000 2>&1 | tail -8)
    done
fi
