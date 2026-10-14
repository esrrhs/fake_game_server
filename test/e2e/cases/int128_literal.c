// expect: 0
// skip_oracle: clang
/* Integer literal typing on LP64: hex/octal wrap at 64 bits; decimal uses
 * the full 128-bit range (int → long → long long → __int128 → unsigned
 * __int128).  Values >= 2^64 are written with shifts so the test behaves
 * identically under both gcc (which wraps) and fakecc (which promotes).
 * Clang rejects hex literals > 2^64-1 outright, so skip under clang. */
package main;

int main(void) {
    int fail = 0;

    if (sizeof(0x1p0) != sizeof(double)) fail = fail + 1;

    /* 2^64 hex wraps to 0, typed as int. */
    if (sizeof(0x10000000000000000) != 4) fail = fail + 1;
    if ((int)0x10000000000000000 != 0) fail = fail + 1;
    unsigned __int128 h = 0x10000000000000000;
    if ((int)h != 0) fail = fail + 1;
    if ((int)(h >> 64) != 0) fail = fail + 1;

    /* 2^64 as a shift (works identically in gcc and fakecc). */
    __int128 d = (__int128)1 << 64;
    if ((int)d != 0) fail = fail + 1;
    if ((int)(d >> 64) != 1) fail = fail + 1;

    /* 2^64 + 0xFF wraps to 0xFF (hex). */
    unsigned __int128 x = 0x100000000000000FF;
    if ((int)x != 0xFF) fail = fail + 1;
    if ((int)(x >> 64) != 0) fail = fail + 1;

    if (sizeof(0x8000000000000000) != 8) fail = fail + 1;
    if (sizeof(0xFFFFFFFFFFFFFFFFULL) != 8) fail = fail + 1;

    /* Decimal 2^63 does not fit long long → __int128 (gcc + fakecc agree). */
    if (sizeof(9223372036854775808) != 16) fail = fail + 1;

    /* 2^64 + 0xFF via shift (identical in gcc and fakecc). */
    unsigned __int128 dx = ((unsigned __int128)1 << 64) + 0xFF;
    if ((int)dx != 0xFF) fail = fail + 1;
    if ((int)(dx >> 64) != 1) fail = fail + 1;

    /* Static initializer with 2^64 + 1 via shift. */
    static __int128 g = ((__int128)1 << 64) + 1;
    if ((int)g != 1) fail = fail + 1;
    if ((int)(g >> 64) != 1) fail = fail + 1;

    /* Decimal 2^64-1 fits in unsigned long long → __int128 in both. */
    if (sizeof(18446744073709551615) != 16) fail = fail + 1;

    return fail;
}
