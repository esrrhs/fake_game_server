// expect: 0
// A 63-bit-wide bitfield exercises the mask helper; building it must not
// overflow the compiler's own shift (found by fuzz_compile through UBSan).
package main;
struct big { unsigned long long a : 63; };
int main(void) { struct big b; b.a = 1; return b.a == 1 ? 0 : 1; }
