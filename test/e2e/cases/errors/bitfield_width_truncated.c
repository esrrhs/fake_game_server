// expect_error
// Truncated bitfield width must be diagnosed, not crash the parser.
package main;
struct S { int a : 