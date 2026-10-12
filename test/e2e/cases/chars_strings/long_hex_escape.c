// expect: 0
// A long \x escape is truncated to the character type; the fold must not
// overflow the compiler's own signed arithmetic (found by fuzz_differential
// through UBSan).
package main;
int main(void) {
    char *s = "\\x1234567890abcdef";
    return s[0] == 0xef ? 1 : 0;
}
