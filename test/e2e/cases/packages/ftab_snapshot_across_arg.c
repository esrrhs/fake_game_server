// expect: 10
/* Regression: FunSig must be snapshotted across recursive argument checks.
 *
 * ftab_lookup() returns a pointer into the function table.  Checking a call
 * argument that resolves an imported `pkg.fn` pushes a new export and can
 * realloc that table, dangling any FunSig* held for the outer callee.
 * Reading the freed slot makes arity / ret_type depend on heap leftovers
 * (Stage 0 vs Stage 1 disagreed on kernel/vfstest.c by 5 bytes; a typical
 * failure mode here is a bogus "left operand of '+' must be arithmetic").
 *
 * Layout: id + p0..p5 + main = 8 FunSigs, which fills the initial cap.
 * The first grow.e0 resolve reallocs 8→16 while id's signature is in use. */
package main;
import grow;

int id(int x) { return x; }
int p0(void) { return 0; }
int p1(void) { return 1; }
int p2(void) { return 2; }
int p3(void) { return 3; }
int p4(void) { return 4; }
int p5(void) { return 5; }

int main(void) {
    return id(grow.e0()) + id(grow.e1());
}
