# FakeCC 测试套件说明

FakeCC 拥有覆盖编译器各个阶段的分层测试体系，包含单元测试（Unit Test）、端到端可执行测试（E2E Test）、GCC 差分测试（Differential Test）、多文件与链接测试、动态库测试、GDB 调试符号测试以及编译器健壮性编译测试。

---

## 1. 目录结构

```
test/
├── CMakeLists.txt          # CMake 测试构建与 CTest 集成定义
├── README.md               # 测试体系说明文档（本文档）
├── test_framework.h/.c     # C 语言单元测试极简断言框架
├── unit/                   # 编译器各子模块单元测试
│   ├── test_lexer.c        # 词法分析测试
│   ├── test_parser.c       # 语法分析与 AST 构建测试
│   ├── test_sema.c         # 语义分析与类型检查测试
│   ├── test_ir.c           # 中间表示（IR）生成与类型降解测试
│   ├── test_cfg.c          # 控制流图（CFG）构建测试
│   ├── test_domtree.c      # 支配树（Dominator Tree）与支配边界测试
│   ├── test_mem2reg.c      # SSA 构造与 mem2reg 变量提升测试
│   ├── test_phi.c          # Phi 节点消除与临界边分割测试
│   ├── test_renaming.c     # SSA 重命名测试
│   ├── test_regalloc.c     # 线性扫描与图着色寄存器分配测试
│   ├── test_codegen.c      # x86-64 汇编代码生成测试
│   ├── test_emit.c         # ELF 二进制目标文件编码测试
│   ├── test_opt.c          # 标量优化（死代码消除、常量传播等）测试
│   ├── test_link.c         # 静态链接器符号解析与重定位测试
│   ├── test_debug.c        # DWARF 调试信息生成测试
│   └── test_pkg.c          # Package 组织与多文件管理测试
├── e2e/                    # 端到端测试体系
│   ├── cases/              # 单文件可执行测试用例集（1,900+ 用例）
│   │   ├── arith/          # 基础算术运算
│   │   ├── control/        # 控制流（if/for/while/switch/goto）
│   │   ├── types/          # 类型系统（整型提升、浮点、_Bool、__int128 等）
│   │   ├── structs/        # 结构体、联合体与位域
│   │   ├── pointers/       # 指针运算、解引用与多级指针
│   │   ├── functions/      # 函数调用、变参（stdarg）与 SysV ABI 传参
│   │   ├── chars_strings/  # 字符与字符串字面量
│   │   ├── runtime/        # 内置独立 libc 库功能（stdio/stdlib/string/ctype）
│   │   ├── gcc_torture/        # GCC C-Torture 经典测试套件
│   │   │   ├── execute/    # GCC Torture 1,653 个执行测试
│   │   │   │   └── ieee/   # GCC 69 个 IEEE 浮点规范测试
│   │   │   ├── builtins/   # GCC 30 个标准库 Builtin 函数测试
│   │   │   ├── compat/     # GCC 7 个结构体/联合体 ABI 兼容性测试
│   │   │   ├── other/      # 复杂回归测试
│   │   │   └── UNSUPPORTED.txt # 记录 FakeCC 故意不支持的 GNU 扩展清单
│   │   └── debug/          # DWARF 调试符号与 GDB 交互测试用例
│   ├── pkg_fixtures/       # 多 Package 依赖结构测试夹具
│   ├── run_e2e.sh          # 单文件 E2E 测试驱动脚本
│   ├── run_difftest.sh     # GCC 差分测试驱动脚本
│   ├── run_multi_e2e.sh    # 多文件编译与静态链接测试驱动脚本
│   ├── run_shlib_e2e.sh    # 动态共享库（.so）链接测试驱动脚本
│   ├── run_gdb_e2e.sh      # 真实 GDB 断点与变量追踪测试驱动脚本
└── compile/                # 编译器健壮性仅编译测试套件（Compile-Only Suite）
    ├── gcc_compile/          # 已支持的编译健壮性用例
    │   ├── *.c                 # 已支持的编译健壮性用例
    │   ├── UNSUPPORTED.txt     # 仍待支持：崩溃 / 超时 / 编译拒绝
    │   └── run_compile.sh      # 健壮性编译驱动脚本（fakecc -c）
```

---

## 2. 各测试套件与执行脚本说明

### ① 单元测试 (`test/unit/`)
* **运行方式**：由 CTest 直接调度运行（`test_lexer`, `test_parser`, `test_ir`, `test_mem2reg` 等）。
* **验证目标**：针对编译器内部各个数据结构和算法提供白盒单元测试覆盖。

### ② 单文件 E2E 测试 (`test/e2e/run_e2e.sh`)
* **测试范围**：`test/e2e/cases/**/*.c`（排除了 `debug/` 目录）。
* **判定机制**：FakeCC 独立编译并执行二进制，比对实际程序退出码与用例头部的 `// expect: <val>` 标注是否一致；对异常用例检查 `// expect_error` 报错拦截。
* **规模**：涵盖 1,900+ 个功能与回归用例。

### ③ GCC 差分测试 (`test/e2e/run_difftest.sh`)
* **测试范围**：`test/e2e/cases/**/*.c` 中所有的正向用例。
* **判定机制**：将同一份源码分别交给宿主 GCC（Oracle 基准）和 FakeCC 编译执行，确保 FakeCC 的输出行为与 GCC 100% 对齐（`gcc_exit == fakecc_exit`）。

### ④ 多文件与静态链接测试 (`test/e2e/run_multi_e2e.sh`)
* **测试范围**：内置 25+ 组多文件协同场景及 `pkg_fixtures/` 模块。
* **验证目标**：跨文件全局符号引用、同包多文件合并、跨 Package 导入导出、以及与 GCC 产出 `.o` 文件的 ABI 混合链接互操作。

### ⑤ 动态链接库测试 (`test/e2e/run_shlib_e2e.sh`)
* **测试范围**：内置 15+ 组动态库链接场景。
* **验证目标**：`-l`、`-L`、`-rpath`、ELF `DT_NEEDED` 依赖标签生成、`-nostdlib` 模式与动态符号解析。

### ⑥ GDB 调试符号测试 (`test/e2e/run_gdb_e2e.sh`)
* **测试范围**：`test/e2e/cases/debug/*.c`。
* **判定机制**：驱动真实的 GDB 调试器加载 `-g` 编译的二进制，依据代码中的 `// BRK` 标记下断点并打出变量值，校验 DWARF 符号与位置列表（Location Lists）正确性。

### ⑦ 编译器健壮性仅编译测试 (`test/compile/run_compile.sh`)
* **测试范围**：`test/compile/gcc_compile/*.c`（2,003 个 GCC 历史复杂压力用例）。
* **判定机制**：对每个用例执行 `fakecc -c $file -o /tmp/xxx.o`，验证 FakeCC 前端与优化 Pass 在面对极其怪异/极端的代码边界时**不会发生崩溃（Crash / 段错误 / ICE）或死循环超时**。

---

## 3. 常用运行命令

### 全量运行 CTest（推荐）
```bash
# 构建并运行全部 25 项测试套件（含 -O0 和 -O1 双级别优化）
cmake -S . -B build -DCMAKE_C_FLAGS="-O2"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### 单独运行指定测试脚本
```bash
# 1. 运行单文件 E2E 全量测试
bash test/e2e/run_e2e.sh ./build/fakecc -O0
bash test/e2e/run_e2e.sh ./build/fakecc -O1

# 2. 运行 GCC 差分测试
bash test/e2e/run_difftest.sh ./build/fakecc -O0

# 3. 运行多文件 / 动态库 / GDB 测试
bash test/e2e/run_multi_e2e.sh ./build/fakecc -O0
bash test/e2e/run_shlib_e2e.sh ./build/fakecc -O0
bash test/e2e/run_gdb_e2e.sh ./build/fakecc

# 4. 运行单文件手动测试
./build/fakecc test/e2e/cases/gcc_torture/execute/gcc_torture_20000112_1.c -o /tmp/test && /tmp/test

# 5. 运行 2,003 个 compile 健壮性编译测试
bash test/compile/gcc_compile/run_compile.sh ./build/fakecc -O0
```

---

## 随机程序差分测试（test/fuzz）

`test/fuzz/gen_random.py SEED {gcc|fakecc}` 按种子生成随机 C 程序（混合位宽/符号整数运算、移位、除法、循环、switch、函数调用等，避免未定义行为），`test/fuzz/run_fuzz.sh [fakecc] [种子数] [起始种子]` 用 gcc 作为 oracle，对比 fakecc `-O0`/`-O1` 的输出。已知会错误编译的程序保存在 `test/fuzz/known_failures/seed<N>.{gcc,fcc}.c`（报告为 XFAIL，不使其失败；修复后会报告 XPASS，可删除该对文件）。CI 的 `fuzz` job 运行该脚本。

生成的辅助函数只读写局部变量和参数，不写全局变量：函数调用可以出现在表达式的任意位置，而 C 未规定表达式各操作数的求值顺序，一旦函数带全局副作用，gcc 与 fakecc 就可能各自"正确地"打印出不同的值（这类程序不是误编译，实测 clang 与 fakecc 一致而与 gcc 不同）。因此输出一旦不一致即为 fakecc 的真实误编译。

脚本还额外兜底这两类"生成器自己写出坏程序"的情况，避免把生成器的 bug 记到 fakecc 头上：oracle 用 `-fsanitize=undefined -fsanitize-trap=undefined` 构建（不需要 libubsan），程序一旦有未定义行为（越界移位、除零、有符号溢出等）就会 trap 并被报为 FAIL；出现差异时再用 clang 复跑一遍，若 clang 与 fakecc 输出一致而 gcc 不同，说明该程序的结果并不唯一（未指定/实现定义行为），报告为 AMBIG 而非 FAIL。

---

## libFuzzer 模糊测试（fuzz）

与上面"生成完整合法程序"互补的一套：基于 libFuzzer + ASan/UBSan 的覆盖率引导变异测试，专找 crash、内存错误和未定义行为。`fuzz/build_fuzz.sh` 构建两个 target（`fuzz_compile` 把任意字节喂给 fakecc 前端，`fuzz_differential` 与 `gcc -fsyntax-only` 比较接受/拒绝），语料从现有测试集抽样生成。用法与 CI 调度见 `fuzz/README.md`。
