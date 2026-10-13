# FakeCC

[![CI](https://github.com/esrrhs/fakecc/actions/workflows/ci.yml/badge.svg)](https://github.com/esrrhs/fakecc/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/esrrhs/fakecc/graph/badge.svg)](https://codecov.io/gh/esrrhs/fakecc)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Language: C99 + GCC Extensions](https://img.shields.io/badge/language-C99%20%2B%20GCC-orange.svg)
![Self-hosting](https://img.shields.io/badge/self--hosting-yes-brightgreen.svg)
![Freestanding](https://img.shields.io/badge/default-zero--dep-brightgreen.svg)

[English Documentation](README.md)

一门类 C 的系统级编程语言与自举优化编译器——**完整支持 C99 标准规范及主流 GCC 语言扩展**，保留 C 语言高效直接的底层执行模型与内存布局，**彻底摒弃 C 语言的预处理器宏（`#define`）与文本头文件（`#include`），转而采用类似 Go 语言的现代 Package 模块体系**，自带轻量零依赖 runtime。

- **已完全自举**：FakeCC 能编译自身源码，两级自举生成产物逐字节完全一致（Fixed Point，`v0/fakecc-1` == `v0/fakecc-2`）；
- **默认零依赖**：用户程序与自举产物默认不链接系统 libc，直接由内置 ELF 生成器与链接器生成静态独立 ELF64 可执行文件；
- **高严苛测试覆盖**：忠实移植 GCC 官方 **`gcc.c-torture/execute` 1,650+ 个完整执行用例**，结合 1,650+ 健壮性编译用例以及 5 个真实开源 C 项目（genann、tinyexpr、tiny-AES-c、tiny-regex-c、tiny-bignum-c）移植，全量测试在 `-O0` 和 `-O1` 下 100% 全部通过；
- **跨平台支持**：同时支持 **Linux x86_64**（ELF64）和 **macOS arm64**（Mach-O），CI 覆盖双平台测试。

---

## 核心设计与语言特性

### 1. 现代 Package 模块系统（替代宏与头文件）

传统 C 语言依赖预处理器进行文本级拼接（`#include`）与宏展开（`#define`），极易引发命名污染、符号冲突以及编译膨胀。FakeCC 彻底移除了预处理器，改用现代模块系统：

- **文件声明包名**：每个源文件顶部通过 `package pkg_name;` 声明所属包；
- **同包自动互见**：同一个 package 目录下的所有源文件自动共享 `typedef`、`struct`、`union`、`enum` 和全局声明，无需维护头文件；
- **包间导入**：跨包调用使用 `import other_pkg;`，在代码中通过限定名访问：
  ```c
  package main;
  import runtime;

  int main(void) {
      runtime.printf("hello %d\n", 42);
      return 0;
  }
  ```
- **包级作用域**：`static` 声明将符号限制在当前包内，外部不可见；非 static 符号导出供其他包使用。

---

### 2. 完整 C99 核心语言支持

FakeCC 完整实现了 C99 标准规范的核心语法与语义特性：

- **基础类型系统**：
  - 整型：`char` / `short` / `int` / `long` / `long long`（及其 `unsigned` 变体）、`_Bool`、`__int128`
  - 浮点：`float`、`double`（SSE 浮点指令）、`long double`（80 位扩展精度 x87 FPU）
  - 复数类型：`_Complex float`、`_Complex double`、`_Complex long double`
  - 派生类型：多级指针、定长数组、多维数组、`struct`、`union`、`enum`、函数指针
  - 类型修饰符：`const`、`volatile`、`restrict`、`inline`
- **C99 进阶特性**：
  - **变长数组（VLA）**：支持运行时动态长度数组分配与多维 VLA（如 `int arr[n][m]`），支持在循环与复杂控制流中跨作用域自动回收栈空间；
  - **复合字面量（Compound Literals）**：如 `(struct Point){ .x = 1, .y = 2 }` 或 `(int[]){ 1, 2, 3 }`；
  - **指定初始化器（Designated Initializers）**：结构体成员指定初始化 `{.field = val}`、数组下标指定初始化 `{[3] = val}` 以及嵌套初始化；
  - **灵活数组成员（Flexible Array Members）**：结构体末尾的 `type array[]` 柔性数组；
  - **声明位置自由**：支持在代码块任意位置声明局部变量，以及 `for (int i = 0; i < n; i++)` 循环头局部变量声明。
- **表达式与控制流**：
  - 完整运算符优先级与隐式类型提升/转换规则（Integer Promotion、Arithmetic Conversions）；
  - `if` / `else`、`switch`（支持稀疏分支、跳表跳转、`case low ... high:` 范围分支与任意跨作用域跳转）、`while`、`do-while`、`for`、`goto`、`break`、`continue`、`return`。

---

### 3. 主流 GCC 扩展支持

为了无缝运行底层系统级代码与复杂的开源测试集，FakeCC 深入支持了主流的 GCC 编译器扩展：

- **语句表达式（Statement Expressions）**：`({ int x = f(); x * 2; })` 允许在表达式内嵌入代码块并返回值；
- **计算跳转（Computed Gotos）**：支持取标签地址 `&&label` 与间接跳转 `goto *expr;`，支持静态标签跳转表；
- **`typeof(...)` 表达式**：支持对表达式和类型的类型求值；
- **GCC 属性系统（`__attribute__`）**：
  - 对齐与打包：`aligned(N)`、`packed`
  - 别名与重命名：`alias("target")`、`__asm__("symbol")`
  - 向量类型：`vector_size(N)`
  - 字节序调整：`scalar_storage_order("big-endian" / "little-endian")`
  - 函数剖析控制：`no_instrument_function`
  - 机器模式：`mode(QI/HI/SI/DI/TI/word/byte)`
- **SIMD 向量扩展**：支持 `__attribute__((vector_size(N)))` 定义的向量类型，支持按元素向量加减乘除、位运算、按元素比较生成全 1/全 0 掩码以及向量初始化；
- **初始化扩展**：数组范围指定初始化器（如 `[0 ... 9] = 1`）；
- **函数级性能剖析**：支持 `-finstrument-functions` 编译选项，在非 `no_instrument_function` 函数的出入口自动注入 `__cyg_profile_func_enter` 与 `__cyg_profile_func_exit` 钩子调用；
- **丰富的内置函数（GCC Builtins）**：
  - 控制流与优化提示：`__builtin_expect`, `__builtin_unreachable`, `__builtin_constant_p`, `__builtin_trap`
  - 位运算指令：`__builtin_clz/clzl/clzll`, `__builtin_ctz/ctzl/ctzll`, `__builtin_popcount/popcountll`, `__builtin_ffs/ffsll`, `__builtin_bswap16/32/64`
  - 溢出安全算术：`__builtin_add_overflow`, `__builtin_sub_overflow`, `__builtin_mul_overflow`（支持 8/16/32/64 位有符号与无符号全类型及 `_p` 变体）
  - 栈与执行上下文：`__builtin_frame_address`, `__builtin_return_address`, `__builtin_stack_save`, `__builtin_stack_restore`, `__builtin_setjmp`, `__builtin_longjmp`, `__builtin_alloca`, `__builtin_va_arg_pack`
  - 内存与标准运算：`__builtin_memcpy`, `__builtin_memset`, `__builtin_memcmp`, `__builtin_abs`, `__builtin_labs`, `__builtin_llabs`, `__builtin_copysign`, `__builtin_inf*`, `__builtin_nan*` 等。

---

## 编译器架构

```
源码 (.c) → Lexer → Tokens → Parser → AST → Sema → SSA IR → 中端优化 (Opt) → Codegen (x86-64) → 内嵌链接器 → ELF64 可执行文件
```

- **前端（Frontend）**：
  - 手写递归下降解析器，直接构建 AST；
  - 语义分析（Sema）负责类型检查、隐式转换插入、常量折叠与作用域符号表解析；
  - AST 降级为中端统一三地址 SSA IR。
- **中端（Middle-end）**：
  - **控制流图（CFG）与支配树（Dominator Tree）**：计算支配前沿与循环结构；
  - **mem2reg**：基于支配前沿自动插入 φ 函数，将栈上的局部标量变量提升至 SSA 寄存器；
  - **标量优化**：常量折叠（Constant Folding）、代数化简、无效代码消除（DCE）、窥孔优化（Peephole）；
  - **寄存器分配**：基于图着色与线性扫描的 SysV AMD64 寄存器分配器（支持通用寄存器与 XMM 寄存器分配、溢出处理与调用约定保存）。
- **后端与链接器（Backend & Linker）**：
  - 原生 x86-64 机器码生成器（不依赖外部 GNU `as`）；
  - 内嵌 ELF64 文件生成器与静态/动态链接器（不依赖 GNU `ld`），直接写出可执行 ELF。

---

## 零依赖 Runtime（`runtime/`）

FakeCC 源码树自带纯 C / FakeCC 实现的独立 runtime（位于 `runtime/` 目录），默认编译时自动将其静态编译并链接入最终二进制中。

| 模块 | 包含的核心能力 |
|---|---|
| `builtin.c` | 基础类型定义（`size_t`, `ssize_t`, `FILE`, `va_list`） |
| `string.c` | `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strcpy`, `strcmp`, `strchr` 等 |
| `ctype.c` | `isdigit`, `isalpha`, `isspace`, `toupper`, `tolower` 等 |
| `malloc.c` | 基于 Linux `mmap` 系统调用的独立内存分配器：`malloc`, `free`, `calloc`, `realloc` |
| `stdio.c` | `stdin`/`stdout`/`stderr` 标准流、用户态 I/O 缓冲区、`fopen`, `fclose`, `fread`, `fwrite`, `fputs` 等 |
| `printf.c` | `printf`, `fprintf`, `sprintf`, `snprintf`, `vprintf`, `vfprintf`, `vsnprintf` |
| `stdlib.c` | `exit`, `abort`, `strtol`, `strtoul`, `qsort`, `getenv`, `abs` |

- **与宿主系统标准库及 GCC 编译产物互操作**：
  - **直接链接 GCC 编译的目标文件（`.o`）**：FakeCC 内置链接器支持直接解析并链接 GCC/Clang 生成的标准 ELF 可重定位目标文件（`.o`），自动完成代码段/数据段符号重定位，完全遵循 SysV AMD64 ABI（结构体传参、返回值、对齐与调用约定）。
  - **由 GCC 链接 FakeCC 编译的目标文件**：使用 `fakecc -c file.c -o file.o` 生成标准 ELF64 目标文件，可直接被 `gcc` 或 `ld` 链接使用。
  - **共享库**：FakeCC 既能 *生成* 共享库（`fakecc -shared … -o libfoo.so`），也能 *链接* 共享库（`-LDIR -lLIB`、`-l:SONAME` 或直接传入 `.so` 路径）。互操作覆盖 fakecc↔fakecc、gcc←fakecc `.so`、以及 fakecc←gcc `.so`。

---

## 自举流程

FakeCC 的自举验证机制保证了编译器自身的逻辑自洽性与稳定性：

```
[ Stage 0 ] gcc 编译 src/*.c                  → build/fakecc
     ↓
[ Stage 1 ] build/fakecc 编译 v0/*.c + runtime → v0/fakecc-1
     ↓
[ Stage 2 ] v0/fakecc-1 再次编译 v0/*.c + runtime → v0/fakecc-2
     ↓
[ 验证 ]   比对 v0/fakecc-1 与 v0/fakecc-2（逐字节 100% 完全一致）
```

运行自举检查：
```bash
bash v0/stage2_check.sh
```

---

## 构建与安装

### 编译构建

```bash
cmake -S . -B build
cmake --build build --parallel
```

### 安装到系统

```bash
# 安装（默认前缀：/usr/local）
sudo cmake --install build
# 或：cd build && sudo make install
```

### 在其他 CMake 项目中使用

安装到系统后，其他 CMake 项目可以通过标准的 `find_package` 引入并链接 FakeCC：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_project C)

# 查找 FakeCC
find_package(fakecc REQUIRED)

add_executable(my_project main.c)

# 链接 fakecc（自动引入头文件路径和链接参数）
target_link_libraries(my_project PRIVATE fakecc::fakecc)
# 同时兼容不带命名空间的别名或 core 写法：
# target_link_libraries(my_project PRIVATE fakecc)
# target_link_libraries(my_project PRIVATE fakecc::fakecc_core)
```

在 C/C++ 代码中引用头文件：

```c
#include "fakecc.h"
// 或者使用带目录前缀的形式：
// #include <fakecc/fakecc.h>
// 或者引用各个模块独立头文件：
// #include <fakecc/compiler.h>
// #include <fakecc/parser.h>
```

#### 高层编译器 C API

FakeCC 提供了开箱即用的一键式 C API，支持直接在内存中将 C 源码字符串或文件编译为 `.so` 动态库、独立可执行文件或 `.o` 目标文件：

```c
#include "fakecc.h"

// 1. 将 C 源码字符串直接编译为 ELF .so 动态链接库：
fakecc_compile_string_to_so(
    "package main;\nint add(int a, int b) { return a + b; }\n",
    "libadd.so",
    NULL /* 默认选项：-O1 优化 */
);

// 2. 将 C 源码字符串直接编译为独立可执行文件：
fakecc_compile_string_to_executable(
    "package main;\nint main(void) { return 42; }\n",
    "my_program",
    NULL
);

// 3. 编译源码文件：
fakecc_compile_file_to_so("math_plugin.c", "libmath.so", NULL);
fakecc_compile_file_to_executable("entry.c", "app", NULL);
fakecc_compile_string_to_obj("int f() { return 0; }", "obj.o", NULL);

// 可选的高级参数配置：
FakeccOptions opts;
fakecc_options_init(&opts);
opts.opt_level = 0;   // -O0
opts.want_debug = 1;  // 生成 DWARF 调试符号
fakecc_compile_file_to_so("plugin.c", "libplugin.so", &opts);
```

FakeCC 还会同时安装 `fakecc.pc`，支持通过 `pkg-config` 使用。

## 运行

```bash
# 编译并运行简单用例
./build/fakecc examples/return42.c -o /tmp/a.out
/tmp/a.out; echo $?    # 42

# 带 DWARF，可用 gdb 按行调试（不影响优化，也不影响生成的指令）
./build/fakecc -g examples/return42.c -o /tmp/a.out

# 变量全部留在栈上，调试体验最直白（-O0 模式）
./build/fakecc -O0 -g examples/return42.c -o /tmp/a.out

# 可选：不用内置 runtime，改链接系统 libc
./build/fakecc hello.c -nostdlib -lc -o /tmp/hello_libc

# 与 GCC 混合编译：FakeCC 直接链接 GCC 生成的 .o 目标文件
gcc -c -O2 helper.c -o /tmp/helper.o
./build/fakecc main.c /tmp/helper.o -o /tmp/mixed_app

# 反向兼容：GCC 直接链接 FakeCC 编译的 .o 目标文件 (-c)
./build/fakecc -c module.c -o /tmp/module.o
gcc main.c /tmp/module.o -o /tmp/mixed_app

# FakeCC 生成共享库，再分别由 FakeCC / GCC 加载：
./build/fakecc -shared math.c -o /tmp/libmath.so
./build/fakecc app.c -L/tmp -lmath -o /tmp/app_fcc
gcc app.c -L/tmp -lmath -Wl,-rpath,/tmp -o /tmp/app_gcc

# 链接 GCC 编译的动态共享库 (.so)：
# 支持 -LDIR、-lNAME、-l:SONAME 以及直接传入 /path/to/lib.so 路径（自动生成 DT_NEEDED 与 DT_RUNPATH）
gcc -shared -fPIC -o /tmp/libmath_gcc.so math.c
./build/fakecc app.c -L/tmp -lmath_gcc -o /tmp/app_with_so
./build/fakecc app.c /tmp/libmath_gcc.so -o /tmp/app_with_so
```

---

## 测试体系

FakeCC 拥有覆盖编译器各个阶段的分层测试体系：

```bash
ctest --test-dir build --output-on-failure
```

CTest 自动调度运行 29 项测试套件（覆盖 `-O0` 和 `-O1`）：
- **单元测试（16 项）**：涵盖词法、语法解析、语义分析、IR、CFG、支配树、Phi 消除、SSA 重命名、mem2reg、优化器、寄存器分配、代码生成、目标文件编码、静态链接、调试信息与 Package 系统；
- **E2E 综合测试（2,600+ 用例）**：覆盖基础算术、控制流、类型系统、复合聚合体、指针、函数调用、字符串、Runtime、SysV ABI 以及 GCC C-Torture 经典测试用例；
- **真实开源项目移植测试（app_ports）**：完整移植并独立运行 `genann`、`tinyaes`、`tinybn`、`tinyexpr`、`tinyregex`；
- **GCC 差分测试（difftest）**：以 GCC 为 Oracle 基准，严格校验 FakeCC 与 GCC 行为输出一致性；
- **多文件与动态库测试**：覆盖跨模块编译、静态链接与 `.so` 动态库加载；
- **编译器健壮性测试（gcc_compile）**：1,650+ 复杂代码仅编译套件；
- **GDB 调试套件**：驱动真实 GDB 校验 DWARF 源码级调试与变量监视。

---

## 性能基准（Benchmark）

独立于单元测试的性能测试体系位于 `bench/` 目录，通过与 GCC 编译产物比对实际运行耗时：

```bash
bash bench/run_bench.sh ./build/fakecc
bash v0/stage2_check.sh && bash bench/run_bench.sh v0/fakecc-1
```

包含 `nbody`（浮点密集计算）、`sieve`（数组与位操作）、`matmul`（整型循环与矩阵乘法）以及 `chacha`（密码学 ARX 运算）。

---

## 开源协议

本项目采用 [MIT 许可证](LICENSE)。
