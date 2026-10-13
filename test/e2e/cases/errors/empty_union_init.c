// expect_error
// Initialising an empty union must be diagnosed, not crash sema.
package main;
typedef union {
} aun;
int main(void) {
    aun a = {{0}};
    return a.a2[2];
}
