# FakeCC

[![CI](https://github.com/esrrhs/fakecc/actions/workflows/ci.yml/badge.svg)](https://github.com/esrrhs/fakecc/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/esrrhs/fakecc/graph/badge.svg)](https://codecov.io/gh/esrrhs/fakecc)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Language: C99 + GCC Extensions](https://img.shields.io/badge/language-C99%20%2B%20GCC-orange.svg)
![Self-hosting](https://img.shields.io/badge/self--hosting-yes-brightgreen.svg)
![Freestanding](https://img.shields.io/badge/default-zero--dep-brightgreen.svg)

[中文说明 (Chinese Documentation)](README_zh.md)

A C-family systems programming language and optimizing self-hosting compiler that **fully supports the C99 standard and mainstream GCC language extensions**. It preserves C's direct, low-level execution model and predictable memory layout, while **completely eliminating the preprocessor (`#define`) and textual headers (`#include`) in favor of a modern, Go-inspired package module system** with a built-in, zero-dependency freestanding runtime.

- **Fully Self-Hosting**: FakeCC compiles its own source code; two-stage bootstrap produces a byte-identical fixed point (`v0/fakecc-1` == `v0/fakecc-2`).
- **Zero Dependencies by Default**: User binaries and bootstrap artifacts link no system libc by default, producing standalone static ELF64 executables directly via its built-in ELF generator and linker.
- **Battle-Tested Test Coverage**: Faithfully ported from official GCC test suites, featuring **1,650+ GCC C-Torture execute tests**, 1,650+ compiler robustness tests, and real-world C projects (genann, tinyexpr, tiny-AES-c, tiny-regex-c, tiny-bignum-c) with 100% pass rates under both `-O0` and `-O1`.
- **Cross-Platform**: Supports both **Linux x86_64** (ELF64) and **macOS arm64** (Mach-O), with CI coverage on both platforms.

---

## Key Design & Language Features

### 1. Modern Package Module System (Replacing Preprocessor & Headers)

Traditional C relies on textual substitution (`#include`) and macro expansion (`#define`), which easily causes namespace pollution, symbol collisions, and slow compilation. FakeCC completely eliminates the preprocessor and introduces a structured package system:

- **Package Declarations**: Each source file begins with `package pkg_name;`.
- **Automatic Same-Package Visibility**: All source files within the same package directory automatically share `typedef`, `struct`, `union`, `enum`, and global declarations without header files.
- **Inter-Package Imports**: Cross-package symbols are accessed via `import other_pkg;` using qualified names:
  ```c
  package main;
  import runtime;

  int main(void) {
      runtime.printf("hello %d\n", 42);
      return 0;
  }
  ```
- **Package-Level Scope**: `static` declarations restrict symbols to the package; non-static declarations are exported to importing packages.

---

### 2. Full C99 Core Language Support

FakeCC provides full implementation of the core C99 specification:

- **Type System**:
  - Integers: `char`, `short`, `int`, `long`, `long long` (signed/unsigned), `_Bool`, `__int128`
  - Floating Point: `float`, `double` (SSE), `long double` (80-bit extended-precision x87 FPU)
  - Complex Numbers: `_Complex float`, `_Complex double`, `_Complex long double`
  - Derived Types: Multilevel pointers, arrays, multidimensional arrays, `struct`, `union`, `enum`, function pointers
  - Type Qualifiers: `const`, `volatile`, `restrict`, `inline`
- **Advanced C99 Features**:
  - **Variable Length Arrays (VLA)**: Dynamic runtime allocation, multidimensional VLAs (e.g. `int arr[n][m]`), and automatic stack deallocation across loop scopes and complex control flow.
  - **Compound Literals**: e.g., `(struct Point){ .x = 1, .y = 2 }` or `(int[]){ 1, 2, 3 }`.
  - **Designated Initializers**: Member designators `{.field = val}`, array index designators `{[3] = val}`, and nested initializers.
  - **Flexible Array Members**: `type array[]` at the end of structures.
  - **Flexible Declarations**: Local declarations anywhere in blocks, and `for (int i = 0; i < n; i++)` loop initializers.
- **Expressions & Control Flow**:
  - Complete operator precedence and integer promotions / usual arithmetic conversions.
  - `if` / `else`, `switch` (supporting sparse cases, dense jump tables, ranges `low ... high`, and cross-scope jumps), `while`, `do-while`, `for`, `goto`, `break`, `continue`, `return`.

---

### 3. Mainstream GCC Language Extensions

To seamlessly compile low-level systems software and open-source packages, FakeCC implements major GCC extensions:

- **Statement Expressions**: `({ int x = f(); x * 2; })` embedding statement blocks inside expressions.
- **Computed Gotos**: Label address-of `&&label` and indirect dispatch `goto *expr;` with jump tables.
- **`typeof(...)`**: Type queries for expressions and types.
- **Attribute System (`__attribute__`)**:
  - Alignment and packing: `aligned(N)`, `packed`
  - Aliasing and renaming: `alias("target")`, `__asm__("symbol")`
  - Vector types: `vector_size(N)`
  - Endianness: `scalar_storage_order("big-endian" / "little-endian")`
  - Function profiling: `no_instrument_function`
  - Machine modes: `mode(QI/HI/SI/DI/TI/word/byte)`
- **SIMD Vector Operations**: Vector arithmetic (`+`, `-`, `*`, `/`), bitwise operations, element-wise comparisons yielding mask vectors, and vector initialization.
- **Designated Array Ranges**: Range initializers like `[0 ... 9] = 1`.
- **Function Profiling (`-finstrument-functions`)**: Injects `__cyg_profile_func_enter` / `__cyg_profile_func_exit` hooks on entry and exit.
- **Extensive GCC Builtins**:
  - Control flow: `__builtin_expect`, `__builtin_unreachable`, `__builtin_constant_p`, `__builtin_trap`
  - Bitwise instructions: `__builtin_clz*`, `__builtin_ctz*`, `__builtin_popcount*`, `__builtin_ffs*`, `__builtin_bswap16/32/64`
  - Overflow-checked arithmetic: `__builtin_add_overflow`, `__builtin_sub_overflow`, `__builtin_mul_overflow` (8/16/32/64-bit signed/unsigned)
  - Stack and context: `__builtin_frame_address`, `__builtin_return_address`, `__builtin_stack_save`, `__builtin_stack_restore`, `__builtin_setjmp`, `__builtin_longjmp`, `__builtin_alloca`, `__builtin_va_arg_pack`
  - Memory & Math: `__builtin_memcpy`, `__builtin_memset`, `__builtin_memcmp`, `__builtin_abs`, `__builtin_labs`, `__builtin_llabs`, `__builtin_copysign`, `__builtin_inf*`, `__builtin_nan*`, etc.

---

## Compiler Architecture

```
Source (.c) → Lexer → Tokens → Parser → AST → Sema → SSA IR → Optimizer (Opt) → Codegen (x86-64) → Embedded Linker → ELF64 Binary
```

- **Frontend**:
  - Handcrafted recursive descent parser building an explicit AST.
  - Semantic analysis (Sema) for type checking, implicit conversions, constant folding, and scoped symbol tables.
  - AST lowering to intermediate three-address SSA IR.
- **Middle-end**:
  - **Control Flow Graph (CFG) & Dominator Tree**: Computes dominance frontiers and loop structures.
  - **mem2reg**: Inserts φ (phi) functions and promotes memory alloca variables into SSA registers.
  - **Scalar Optimizations**: Constant propagation and folding, algebraic simplifications, dead code elimination (DCE), and peephole rewrites.
  - **Register Allocator**: Graph coloring and linear scan allocator conforming to SysV AMD64 ABI (handling GP and XMM registers, spills, and calling convention preservation).
- **Backend & Linker**:
  - Native x86-64 instruction emitter (no external assembler `as` required).
  - Built-in ELF64 object/executable generator and static/dynamic linker (no external `ld` required).

---

## Zero-Dependency Runtime (`runtime/`)

FakeCC includes a standalone runtime written in pure C/FakeCC located in `runtime/`. It is automatically compiled and linked into final binaries by default.

| Module | Core Capabilities |
|---|---|
| `builtin.c` | Basic types (`size_t`, `ssize_t`, `FILE`, `va_list`) |
| `string.c` | `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strcpy`, `strcmp`, `strchr`, etc. |
| `ctype.c` | `isdigit`, `isalpha`, `isspace`, `toupper`, `tolower`, etc. |
| `malloc.c` | Standalone allocator on Linux `mmap` syscall: `malloc`, `free`, `calloc`, `realloc` |
| `stdio.c` | `stdin`/`stdout`/`stderr` streams, buffered I/O, `fopen`, `fclose`, `fread`, `fwrite`, `fputs`, etc. |
| `printf.c` | `printf`, `fprintf`, `sprintf`, `snprintf`, `vprintf`, `vfprintf`, `vsnprintf` |
| `stdlib.c` | `exit`, `abort`, `strtol`, `strtoul`, `qsort`, `getenv`, `abs` |

- **Interoperability with System Libs & GCC Object Files**:
  - **Linking GCC Object Files (`.o`)**: FakeCC's built-in linker can directly parse and link standard ELF relocatable object files (`.o`) generated by GCC/Clang, resolving text/data relocations and conforming to the SysV AMD64 ABI (structure passing/returns, alignment, calling conventions).
  - **Consuming FakeCC Object Files with GCC**: `fakecc -c file.c -o file.o` produces standard ELF64 relocatable object files that can be directly passed to `gcc` or `ld`.
  - **Shared Libraries**: FakeCC can both *produce* shared objects (`fakecc -shared … -o libfoo.so`) and *consume* them (`-LDIR -lLIB`, `-l:SONAME`, or a direct `.so` path). Interop covers fakecc↔fakecc, gcc←fakecc `.so`, and fakecc←gcc `.so`.

---

## Bootstrap & Self-Hosting

FakeCC's self-hosting verification ensures compiler correctness and reproducibility:

```
[ Stage 0 ] Host GCC compiles src/*.c                 → build/fakecc
     ↓
[ Stage 1 ] build/fakecc compiles v0/*.c + runtime    → v0/fakecc-1
     ↓
[ Stage 2 ] v0/fakecc-1 recompiles v0/*.c + runtime   → v0/fakecc-2
     ↓
[ Verify  ] Compare v0/fakecc-1 and v0/fakecc-2 (100% byte-identical fixed point)
```

Run bootstrap verification:
```bash
bash v0/stage2_check.sh
```

---

## Building & Installation

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Install to System

```bash
# Install (default prefix: /usr/local)
sudo cmake --install build
# or: cd build && sudo make install
```

### Using FakeCC in Other CMake Projects

After installing to your system, other CMake projects can discover and link FakeCC using standard `find_package`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_project C)

# Discover FakeCC package
find_package(fakecc REQUIRED)

add_executable(my_project main.c)

# Link against fakecc (automatically sets up include directories and link flags)
target_link_libraries(my_project PRIVATE fakecc::fakecc)
# Alternatively, the unqualified alias or core name is also supported:
# target_link_libraries(my_project PRIVATE fakecc)
# target_link_libraries(my_project PRIVATE fakecc::fakecc_core)
```

In your C/C++ code:

```c
#include "fakecc.h"
// or
#include <fakecc/fakecc.h>
// or individual modular headers
#include <fakecc/compiler.h>
#include <fakecc/parser.h>
```

#### High-Level Compiler C API

FakeCC provides easy-to-use C APIs to compile code strings or files into `.so` shared libraries, standalone executables, or `.o` object files directly:

```c
#include "fakecc.h"

// 1. Compile C source string directly to an ELF shared library (.so):
fakecc_compile_string_to_so(
    "package main;\nint add(int a, int b) { return a + b; }\n",
    "libadd.so",
    NULL /* default options: -O1 */
);

// 2. Compile C source string to a standalone executable:
fakecc_compile_string_to_executable(
    "package main;\nint main(void) { return 42; }\n",
    "my_program",
    NULL
);

// 3. Compile source files:
fakecc_compile_file_to_so("math_plugin.c", "libmath.so", NULL);
fakecc_compile_file_to_executable("entry.c", "app", NULL);
fakecc_compile_string_to_obj("int f() { return 0; }", "obj.o", NULL);

// Optional customization:
FakeccOptions opts;
fakecc_options_init(&opts);
opts.opt_level = 0;   // -O0
opts.want_debug = 1;  // emit DWARF debug symbols
fakecc_compile_file_to_so("plugin.c", "libplugin.so", &opts);
```

FakeCC also installs `fakecc.pc` for `pkg-config` consumers.

## Usage

```bash
# Compile and run a basic program
./build/fakecc examples/return42.c -o /tmp/a.out
/tmp/a.out; echo $?    # 42

# Emit DWARF debug information for GDB source-level stepping
./build/fakecc -g examples/return42.c -o /tmp/a.out

# Keep locals in memory (-O0)
./build/fakecc -O0 -g examples/return42.c -o /tmp/a.out

# Optional: link against system glibc instead of builtin runtime
./build/fakecc hello.c -nostdlib -lc -o /tmp/hello_libc

# Mixed compilation with GCC: FakeCC links GCC object files (.o)
gcc -c -O2 helper.c -o /tmp/helper.o
./build/fakecc main.c /tmp/helper.o -o /tmp/mixed_app

# Conversely, GCC can link object files generated by FakeCC (-c)
./build/fakecc -c module.c -o /tmp/module.o
gcc main.c /tmp/module.o -o /tmp/mixed_app

# Produce a shared library with FakeCC, then load it from FakeCC or GCC:
./build/fakecc -shared math.c -o /tmp/libmath.so
./build/fakecc app.c -L/tmp -lmath -o /tmp/app_fcc
gcc app.c -L/tmp -lmath -Wl,-rpath,/tmp -o /tmp/app_gcc

# Linking dynamic shared libraries (.so) built by GCC:
# Supports -LDIR, -lNAME, -l:SONAME, and direct /path/to/lib.so (generates DT_NEEDED & DT_RUNPATH)
gcc -shared -fPIC -o /tmp/libmath_gcc.so math.c
./build/fakecc app.c -L/tmp -lmath_gcc -o /tmp/app_with_so
./build/fakecc app.c /tmp/libmath_gcc.so -o /tmp/app_with_so
```

---

## Testing

FakeCC maintains a comprehensive testing pipeline:

```bash
ctest --test-dir build --output-on-failure
```

CTest automatically runs 29 test suites across `-O0` and `-O1`:
- **Unit Tests (16 suites)**: Lexer, parser, sema, IR, CFG, domtree, phi, renaming, mem2reg, opt, regalloc, codegen, emit, link, debug, and package system.
- **E2E Test Suites (2,600+ cases)**: Arithmetic, control flow, types, aggregates, pointers, functions, strings, runtime, SysV ABI, and GCC C-Torture execute suite.
- **Real-world App Ports**: Independent compilation and verification of `genann`, `tinyaes`, `tinybn`, `tinyexpr`, and `tinyregex`.
- **GCC Differential Testing (`difftest`)**: Exact output and exit code comparisons against host GCC.
- **Multi-File & Shared Library Interop**: Linking multiple object files, packages, and `.so` dynamic libraries.
- **Compiler Robustness Suite (`gcc_compile`)**: 1,650+ complex torture compile-only cases.
- **GDB Debugger Suite**: Verifies DWARF line numbers and variable inspection using interactive GDB scripts.

---

## Benchmark

A dedicated benchmark suite resides in `bench/` (comparing binary runtime performance against GCC `-O0`, `-O1`, `-O2`):

```bash
bash bench/run_bench.sh ./build/fakecc
bash v0/stage2_check.sh && bash bench/run_bench.sh v0/fakecc-1
```

Includes `nbody` (floating-point computation), `sieve` (array and bit operations), `matmul` (nested loops and integer memory access), and `chacha` (ARX cryptographic primitives).

---

## License

FakeCC is open source under the [MIT License](LICENSE).
