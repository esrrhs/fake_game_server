// expect_error
// A declarator whose '(' is never closed runs the paren scanner off the end
// of the token array.  It must be diagnosed, not read past the array (the
// token's loc.file used to be garbage, which crashed printing the diagnostic).
package main;
int main() { int (x = 1; return 0; }
