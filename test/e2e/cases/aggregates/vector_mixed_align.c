// expect: 0
// vector_size(32) and vector_size(64) side by side in one frame.
//
// vector32_align and vector64_align each hold a single over-aligned local.
// This one mixes alignments with a plain scalar in between, which is what
// catches the frame-layout mistake: an over-aligned slot is addressed as
// align_down(fp + off, aln), so it can resolve up to aln-1 bytes below the
// offset the frame plan recorded.  When every slot masked to its own
// alignment, a 64-byte and a 32-byte slot overlapped by 32 bytes whenever
// fp landed at 32 or 48 mod 64 -- and the failure came and went with the
// stack, so a single run could not see it.
//
// Run this a few times: a layout bug here is alignment-dependent.
package main;
typedef int V32 __attribute__((vector_size(32)));
typedef int V64 __attribute__((vector_size(64)));

struct A {
    char c;
    V64 v;
};

int both(int n) {
    struct A a;
    V32 w[2];
    int plain = 5;
    a.c = 3;
    plain = n + 5;
    for (int i = 0; i < 16; i++) a.v[i] = i + n;
    for (int i = 0; i < 16; i++) w[i / 8][i % 8] = i + n;

    if (((unsigned long)&a.v & 63ul) != 0) return 1;
    if (((unsigned long)&w[0] & 31ul) != 0) return 2;
    if (((unsigned long)&w[1] & 31ul) != 0) return 3;

    int sum = 0;
    for (int i = 0; i < 16; i++) sum += a.v[i];
    for (int i = 0; i < 16; i++) sum += w[i / 8][i % 8];
    /* n=1: sum over i+n twice = 2*(16+136)/... = 2*136 = 272 */
    if (n == 1 && sum != 272) return 4;
    /* n=2: 2*(2+...+17) = 2*152 = 304 */
    if (n == 2 && sum != 304) return 5;

    /* The scalar sits between the two vector slots in the frame, so a slot
     * resolving low enough to overlap it corrupts plain instead. */
    if (plain != n + 5) return 6;
    if (a.c != 3) return 7;
    return 0;
}

int main(void) {
    if (both(1) != 0) return 10;
    if (both(2) != 0) return 11;
    /* Repeat in one frame chain: if the layout were unsound the damage would
     * show up as a corrupted earlier value rather than a fresh one. */
    int acc = 0;
    for (int i = 0; i < 8; i++) acc += both(1);
    if (acc != 0) return 12;
    return 0;
}