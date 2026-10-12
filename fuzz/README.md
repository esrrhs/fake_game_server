# FakeCC Fuzz Testing

基于 [libFuzzer](https://llvm.org/docs/LibFuzzer.html) 的模糊测试：用覆盖率引导的变异输入去撞编译器前端与类型/布局计算，找出固定测试用例集永远撞不到的 crash、内存错误和未定义行为。

## CI 流水线

`.github/workflows/fuzz.yml`：

| 触发条件 | 每个 target 运行时长 |
|----------|---------------------|
| Pull Request | 60 秒（快速冒烟） |
| Push to master | 10 分钟 |
| 每周定时 (周日 4:37 UTC) | 60 分钟 |
| 手动触发 (`workflow_dispatch`) | 可配置 |

两个 target 并行运行，发现 crash 时自动上传 crash 输入与日志。

## 架构

```
fuzz/
├── build_fuzz.sh          # 构建脚本（build / test / clean）
├── fuzz_compile.c         # 进程内把任意字节喂给 fakecc 前端
├── fuzz_differential.c    # 与 gcc -fsyntax-only 比较接受/拒绝
├── c_keywords.dict        # libFuzzer 字典（C 关键字、运算符、预处理指令）
└── README.md
```

## Fuzz Target 说明

### fuzz_compile — 编译器健壮性

把输入字节当作 C 源码，走完整流水线（lexer → parser → sema → IR → opt → codegen → ELF 发射），
编译到 `.o`（不链接，不需要 `runtime/`）。检测：

- Lexer / Parser / Sema / IR / codegen 中的 crash / segfault
- ASan 检测的内存错误（越界读写、use-after-free、use-after-scope）
- UBSan 检测的未定义行为

编译失败是预期行为（输入本来就是随机字节），任何其他崩溃都是 bug。`libfakecc` 在错误路径上会
`exit(1)` 且不释放内存，所以开启了 `detect_leaks=0`，不把内存泄漏当成 bug。

### fuzz_differential — 与 gcc 的诊断一致性

同一个源码交给 fakecc 与 `gcc -fsyntax-only`，比较"接受还是拒绝"。判定是单向的：fakecc 只实现了
gcc C 的子集，比 gcc 拒绝得更多属于正常，只有 **fakecc 接受而 gcc 拒绝** 才算 bug。

值级别的差分（编译通过但算错）由 `test/fuzz/` 的生成式差分负责——libFuzzer 的随机字节几乎变不出
有语义的程序，而那边每次都生成完整、无 UB 的 C 程序。

## 构建与运行

```bash
./fuzz/build_fuzz.sh            # 构建（clang + libFuzzer + ASan/UBSan）
./fuzz/build_fuzz.sh test       # 构建并各跑 1000 次
./fuzz/build_fuzz.sh clean
```

语料不需要额外维护：`build_fuzz.sh` 从现有测试集里抽样生成（e2e 用例 + gcc torture 用例给
`fuzz_compile`，纯 C 的 gcc torture 用例给 `fuzz_differential`），比手写几个种子强得多。

```bash
cd build_fuzz
ASAN_OPTIONS=detect_leaks=0 ./bin/fuzz_compile corpus/compile \
    -dict=../fuzz/c_keywords.dict -max_len=4096 -runs=1000000
```

`fuzz_compile` 约 150 exec/s；`fuzz_differential` 每次要 fork gcc，慢一到两个数量级，只适合长跑。

## 发现 bug 后

1. libFuzzer 会把触发输入写到当前目录（`crash-<hash>`、`oom-<hash>`、`timeout-<hash>`）；
2. 最小化：`./bin/fuzz_compile crash-<hash> -minimize_crash=1 -runs=100000`；
3. 定位：`gdb --args ./bin/fuzz_compile minimized-crash`（或让 UBSan 直接打印栈：
   `UBSAN_OPTIONS=print_stacktrace=1`）；
4. 修复后把最小化输入放到 `test/e2e/cases/` 或 `test/compile/gcc_compile/` 作为回归用例。
