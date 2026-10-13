#!/usr/bin/env python3
"""Random C program generator for differential testing (gcc is the oracle).

usage: gen_random.py SEED {gcc|fakecc}

The same SEED yields the same program for both targets; only the prologue and
the print call differ.  Programs use integer arithmetic of mixed widths and
signedness, casts, shifts, division, comparisons, ternaries, loops, switch,
static functions and global arrays.  All arithmetic is done in unsigned types
and shifts/divisors are masked so the programs avoid undefined behaviour.
They print every global/local/array element so any miscompile shows up in
stdout.

The helper functions never write a global: a call may sit anywhere inside an
expression and C leaves the order in which the operands of an expression are
evaluated unspecified, so a call with side effects would let both compilers be
"right" while printing different values.  Keeping the helpers free of global
stores makes the printed values depend only on the program, not on the order.

run_fuzz.sh enforces both invariants instead of trusting this generator: the
oracle is built with UBSan in trap mode, so a program with undefined behaviour
dies instead of printing a value, and a difference is re-checked against clang
(reported AMBIG when clang prints what fakecc prints).
"""
import random
import sys

TYPES = [("signed char", 8), ("unsigned char", 8), ("short", 16),
         ("unsigned short", 16), ("int", 32), ("unsigned int", 32),
         ("long long", 64), ("unsigned long long", 64)]
U32 = ("unsigned int", 32)


def main():
    seed = int(sys.argv[1])
    target = sys.argv[2]
    R = random.Random(seed)

    def T():
        return R.choice(TYPES)

    def wide(t):  # unsigned type used to perform arithmetic for result type t
        return "unsigned long long" if t[1] == 64 else "unsigned int"

    def lit(t):
        v = R.choice([0, 1, 2, 3, 7, 15, 100, 127, 128, 255, 256, 32767,
                      65535, R.getrandbits(t[1]), R.getrandbits(8)])
        return "(%s)%dull" % (t[0], v)

    gl = [("g%d" % i, T()) for i in range(R.randint(6, 12))]
    arr = [("ga%d" % i, T(), R.randint(3, 8)) for i in range(2)]
    funcs = []

    def expr(scope, depth, t=None):
        t = t or T()
        W = wide(t)
        pool = gl + scope
        if depth <= 0 or R.random() < 0.15:
            c = R.random()
            if c < 0.4:
                return lit(t)
            if c < 0.8:
                return "(%s)%s" % (t[0], R.choice(pool)[0])
            a = R.choice(arr)
            return "(%s)%s[%d]" % (t[0], a[0], R.randrange(a[2]))
        c = R.random()
        e = lambda tt=None: expr(scope, depth - 1, tt)
        if c < 0.35:
            op = R.choice(["+", "-", "*", "&", "|", "^"])
            return "(%s)((%s)%s %s (%s)%s)" % (t[0], W, e(), op, W, e())
        if c < 0.45:
            op = R.choice(["/", "%"])
            return "(%s)((%s)%s %s ((%s)%s | 1u))" % (t[0], W, e(), op, W, e())
        if c < 0.55:
            op = R.choice(["<<", ">>"])
            return "(%s)((%s)%s %s ((unsigned)%s & %d))" % (
                t[0], W, e(), op, e(U32), 63 if W == "unsigned long long" else 31)
        if c < 0.65:
            return "(%s)(%s ? %s : %s)" % (t[0], cond(scope, depth - 1), e(), e())
        if c < 0.72:
            return "(%s)(%s)" % (t[0], cond(scope, depth - 1))
        if c < 0.8:
            return "(%s)(~(%s)%s)" % (t[0], W, e())
        if c < 0.85:
            return "(%s)((%s)0 - (%s)%s)" % (t[0], W, W, e())
        if funcs and c < 0.95:
            f = R.choice(funcs)
            return "(%s)%s(%s)" % (t[0], f[0], ", ".join(
                expr(scope, depth - 1, p) for p in f[2]))
        return "(%s)%s" % (t[0], R.choice(pool)[0])

    def cond(scope, depth):
        c = R.random()
        if c < 0.5:
            return "(%s %s %s)" % (expr(scope, max(depth, 0)),
                                   R.choice(["<", "<=", ">", ">=", "==", "!="]),
                                   expr(scope, max(depth, 0)))
        if c < 0.7:
            return "(%s && %s)" % (cond(scope, depth - 1), cond(scope, depth - 1))
        if c < 0.85:
            return "(%s || %s)" % (cond(scope, depth - 1), cond(scope, depth - 1))
        if c < 0.92:
            return "(!%s)" % cond(scope, depth - 1)
        return "(%s)" % expr(scope, max(depth, 0))

    # globals the function being generated is allowed to store into; empty for
    # the static helpers (see the module docstring), all globals inside main
    pool = []

    def assign(scope):
        if pool and R.random() < 0.15:
            a = R.choice(arr)
            return "%s[%d] = %s;" % (a[0], R.randrange(a[2]), expr(scope, 3, a[1]))
        nm, ty = R.choice([v for v in pool + scope if v[0][0] not in "iw"])
        return "%s = %s;" % (nm, expr(scope, R.randint(1, 4), ty))

    def stmt(scope, depth, ind, n):
        p = "    " * ind
        c = R.random()
        if depth <= 0 or c < 0.5:
            return p + assign(scope) + "\n"
        if c < 0.65:
            s = p + "if %s {\n" % cond(scope, 2)
            for _ in range(R.randint(1, 3)):
                s += stmt(scope, depth - 1, ind + 1, n)
            if R.random() < 0.5:
                s += p + "} else {\n"
                for _ in range(R.randint(1, 3)):
                    s += stmt(scope, depth - 1, ind + 1, n)
            return s + p + "}\n"
        if c < 0.8:
            v = "i%d" % n[0]
            n[0] += 1
            s = p + "for (int %s = 0; %s < %d; %s++) {\n" % (v, v, R.randint(1, 6), v)
            sc = scope + [(v, ("int", 32))]
            for _ in range(R.randint(1, 3)):
                s += stmt(sc, depth - 1, ind + 1, n)
            return s + p + "}\n"
        if c < 0.9:
            s = p + "switch ((int)(%s) & 7) {\n" % expr(scope, 2, ("int", 32))
            for k in sorted(R.sample(range(8), R.randint(2, 4))):
                s += p + "case %d:\n" % k + stmt(scope, depth - 1, ind + 1, n)
                if R.random() < 0.7:
                    s += p + "    break;\n"
            return s + p + "default:\n" + stmt(scope, depth - 1, ind + 1, n) + p + "}\n"
        v = "w%d" % n[0]
        n[0] += 1
        s = p + "{ int %s = %d; while (%s > 0) {\n" % (v, R.randint(1, 5), v)
        sc = scope + [(v, ("int", 32))]
        s += stmt(sc, depth - 1, ind + 1, n)
        return s + p + "    %s--;\n" % v + p + "} }\n"

    out = []
    if target == "gcc":
        out.append("#include <stdio.h>\n#define PR(n, v) printf(n \"=%llx\\n\", (unsigned long long)(v))")
    else:
        out.append("package main;\nimport runtime;")
    for nm, t in gl:
        out.append("%s %s = %s;" % (t[0], nm, lit(t)))
    for nm, t, k in arr:
        out.append("%s %s[%d] = {%s};" % (t[0], nm, k, ", ".join(lit(t) for _ in range(k))))
    for fi in range(R.randint(2, 4)):
        pt = [T() for _ in range(R.randint(1, 4))]
        rt = T()
        params = [("p%d" % i, t) for i, t in enumerate(pt)]
        loc = [("l%d" % i, T()) for i in range(R.randint(1, 3))]
        sc = params + loc
        s = "static %s f%d(%s) {\n" % (rt[0], fi, ", ".join("%s %s" % (t[0], n) for n, t in params))
        for n, t in loc:
            s += "    %s %s = %s;\n" % (t[0], n, expr(params, 2, t))
        cnt = [0]
        for _ in range(R.randint(2, 6)):
            s += stmt(sc, 2, 1, cnt)
        s += "    return %s;\n}\n" % expr(sc, 2, rt)
        out.append(s)
        funcs.append(("f%d" % fi, rt, pt))
    m = "int main(void) {\n"
    pool = gl
    mloc = [("m%d" % i, T()) for i in range(R.randint(2, 4))]
    for n, t in mloc:
        m += "    %s %s = %s;\n" % (t[0], n, lit(t))
    cnt = [0]
    for _ in range(R.randint(6, 16)):
        m += stmt(mloc, 3, 1, cnt)
    pr = 'PR("%s", %s);' if target == "gcc" else 'runtime.printf("%s=%%llx\\n", (unsigned long long)%s);'
    for n, t in gl + mloc:
        m += "    " + pr % (n, n) + "\n"
    for a in arr:
        for k in range(a[2]):
            m += "    " + pr % ("%s_%d" % (a[0], k), "%s[%d]" % (a[0], k)) + "\n"
    out.append(m + "    return 0;\n}\n")
    print("\n".join(out))


main()
