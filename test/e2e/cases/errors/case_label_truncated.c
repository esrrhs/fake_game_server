// expect_error
// Truncated case label must be diagnosed, not crash the parser.
package main;
int main() {
    int x = 1;
    switch (x) {
    case 