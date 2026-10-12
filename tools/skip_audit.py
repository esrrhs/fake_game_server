#!/usr/bin/env python3
"""Drive the remaining darwin-arm64 skips one at a time.

Each entry in SKIPS is a case the suite marks `unsupported_on: darwin-arm64`.
For each one this script asks a single question: can the host toolchain run it?
A case the host cannot run either is a genuine platform constraint and stays
skipped; one the host passes is real work for us.  It writes a verdict table
so the answer is reproducible instead of a judgement call in someone's head.

Usage:
    python3 tools/skip_audit.py            # classify every skip
    python3 tools/skip_audit.py <name>     # detail for one case
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "test", "e2e", "cases")
FAKECC = os.path.join(ROOT, "build-local", "fakecc")

# fakecc's dialect: `package main;` / `import x;` have no C equivalent, so a
# host compiler cannot read these files as-is.  Strip the package clause and
# turn imports into a local namespace so the C core can still be compiled --
# that is enough to tell a platform constraint from our own missing feature.
STRIP = [
    (re.compile(r"^\s*package\s+\w+\s*;"), ""),
    (re.compile(r"^\s*import\s+(\w+)\s*;"), r"/* import \1 */"),
]


def parse_directives(path):
    d = {"flags": []}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^//\s*(expect|gcc_flags|flags|link|libs|expect_error)\s*:\s*(.*)$", line)
        if m:
            key, val = m.group(1), m.group(2).strip()
            if key == "expect":
                d[key] = val
            else:
                d["flags"] += val.split()
    return d


def to_c(path, out):
    """Rewrite the fakecc dialect away, keeping the C body intact.

    `import runtime;` makes `runtime.foo` resolve to plain `foo`, so the
    equivalent C source just drops the qualifier.  Leaving it in place makes
    clang report an undeclared identifier and the case then looks like a
    platform constraint for entirely the wrong reason.
    """
    src = open(path, encoding="utf-8", errors="replace").read()
    src = re.sub(r"^\s*package\s+\w+\s*;", "", src, flags=re.M)
    namespaces = re.findall(r"^\s*import\s+(\w+)\s*;", src, flags=re.M)
    src = re.sub(r"^\s*import\s+\w+\s*;", "", src, flags=re.M)
    for ns in dict.fromkeys(namespaces):
        src = re.sub(rf"\b{ns}\.(?=[A-Za-z_])", "", src)
    open(out, "w", encoding="utf-8").write(src)


def host_trial(path, name):
    """Compile with clang, run, compare against `// expect:`."""
    csrc = f"/tmp/skip_audit_{name}.c"
    exe = f"/tmp/skip_audit_{name}.bin"
    to_c(path, csrc)
    d = parse_directives(path)
    # Thread cases name pthread_* and thread_t from the fakecc runtime; the
    # host has the real ones, so map the two fakecc-only spellings onto it.
    hdr = ""
    if re.search(r"\bthread_t\b", open(csrc).read()):
        hdr = ("typedef pthread_t thread_t;\n"
               "static int thread_create(thread_t *t, void *(*f)(void *), void *a)"
               " { return pthread_create(t, 0, f, a); }\n"
               "static int thread_join(thread_t t, void **r) { return pthread_join(t, r); }\n")
        body = open(csrc).read()
        open(csrc, "w").write("#include <pthread.h>\n" + hdr + body)
    cc = subprocess.run(["clang", "-x", "c", "-w", csrc, "-o", exe] + d["flags"],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        first = (cc.stderr.strip().splitlines() or [""])[0]
        return "host-compile-fail", first[:70]
    try:
        r = subprocess.run([exe], capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return "host-timeout", ""
    want = d.get("expect", "0")
    ok = str(r.returncode) == want
    return ("host-pass" if ok else "host-fail"), f"rc={r.returncode} want={want}"


def fakecc_trial(path, name, opt="-O1"):
    out = f"/tmp/skip_audit_{name}.fakecc"
    d = parse_directives(path)
    cc = subprocess.run([FAKECC, opt, path, "-o", out] + d["flags"],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        first = (cc.stderr.strip().splitlines() or [""])[0]
        return "fakecc-compile-fail", first[:70]
    try:
        r = subprocess.run([out], capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return "fakecc-timeout", ""
    want = d.get("expect", "0")
    ok = str(r.returncode) == want
    return ("fakecc-pass" if ok else "fakecc-fail"), f"rc={r.returncode} want={want}"


def collect():
    skips = []
    for dirpath, _, files in os.walk(CASES):
        for f in files:
            if not f.endswith(".c"):
                continue
            p = os.path.join(dirpath, f)
            head = open(p, encoding="utf-8", errors="replace").read(2000)
            if "unsupported_on: darwin-arm64" in head:
                skips.append(os.path.relpath(p, CASES))
    return sorted(skips)


def main():
    skips = collect()
    if len(sys.argv) > 1:
        target = [s for s in skips if sys.argv[1] in s]
        for s in target:
            name = os.path.basename(s)[:-2]
            hv, hd = host_trial(os.path.join(CASES, s), name)
            fv, fd = fakecc_trial(os.path.join(CASES, s), name)
            print(f"{s}\n  host : {hv:20s} {hd}\n  fakecc: {fv:19s} {fd}")
        return

    print(f"{len(skips)} case(s) skipped on darwin-arm64\n")
    print(f"{'case':58s} {'host':22s} verdict")
    print("-" * 100)
    actionable = []
    for s in skips:
        name = os.path.basename(s)[:-2]
        hv, hd = host_trial(os.path.join(CASES, s), name)
        if hv == "host-pass":
            fv, fd = fakecc_trial(os.path.join(CASES, s), name)
            verdict = "ACTIONABLE" if fv != "fakecc-pass" else "already works"
            if fv != "fakecc-pass":
                actionable.append((s, fv, fd))
        else:
            verdict = "platform" if "compile" in hv else hv
        print(f"{s:58s} {hv:22s} {verdict}")

    print()
    if actionable:
        print(f"{len(actionable)} case(s) the host runs and we do not:")
        for s, fv, fd in actionable:
            print(f"  {s}: {fv} ({fd})")
    else:
        print("Nothing actionable: every skip fails on the host toolchain too.")


if __name__ == "__main__":
    main()
