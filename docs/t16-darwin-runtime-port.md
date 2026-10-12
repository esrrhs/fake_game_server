> **⚠️ 本文是 2026-10-05 的快照，大量内容已过时。**
> 当前真实状态（2026-10-12）：**e2e 2672 通过 / 0 失败 / 13 跳过**，
> 单元测试 26/26。下面的 syscall 号码转译、多 TU 重定位等都已完成，
> 「🔴 下一步」里说的 `a[2]` 读成 `a[0]` 等问题**均已修复**。
> 剩下的 13 个跳过项全是平台/设计硬约束，逐条列在
> `.workbuddy/memory/MEMORY.md` 的基线表里。
> 本文档保留作为**排查手法与坐标系陷阱**的参考（那部分仍然有效）。

# T16：arm64 的 runtime 移植方案

## 目标
让 `runtime/`（4599 行，11 个文件）在 Darwin arm64 上可用，从而给 arm64
用户程序提供 `printf` / `fopen` / `strtod` / `sscanf` 等 libc 服务。

**这是 arm64 剩余 e2e 失败的最大来源**：224 个失败里约 120 个是
`undefined function 'printf' / 'sprintf' / 'fopen' / ...`。

## 当前阻塞点：syscall 号码是 Linux 的
`runtime/*.c` 用 `__syscall(N, ...)` 直接发 Linux x86-64 号码，编译器原样透传。
Darwin arm64 的号码完全不同，于是**静默执行了错误的系统调用**：

| runtime 写的 N | Linux 含义 | Darwin arm64 实际含义 | 后果 |
|---|---|---|---|
| 1 | write | **exit** | printf 输出时直接退出进程 |
| 2 | open | 无效 | 失败 |
| 3 | close | 无效 | 失败 |
| 60 | exit | 无效 | 失败 |
| 9 | mmap | 无效 | malloc 全面失败 |
| 11 | munmap | 无效 | free 失败 |

实测证据：`printf("hello %d\n", 42)` 产生 rc=138，而 `exit(138)` 也正好是
rc=138 —— 确认 `__syscall(1)` 执行的是 Darwin 的 exit。

⚠️ **这类错误不会编译失败，也不会明显崩溃，而是静默产生错误行为。**
不要通过"解除 runtime 门控"来试���，那样会让所有程序输出错误结果。

## 号码对照表（Darwin arm64）
已确认的映射（Darwin arm64 syscall 编号）：

| syscall | Darwin | 备注 |
|---|---|---|
| read | 3 | cg64 已有 builtin |
| write | 4 | cg64 已有 builtin |
| open | 5 | cg64 已有 builtin |
| close | 6 | cg64 已有 builtin |
| exit | 1 | cg64 emit_exit_builtin 已实现 |
| getpid | 20 | |
| mmap | 197 | |
| munmap | 73 | |
| lseek | 199 | |
| unlink | 10 | runtime 87 |
| dup2 | 90 | 号码巧合相同 |
| gettid | 224 | |
| futex | — | **无等价**，需换实现 |
| clone | — | **无等价**，需换实现 |

## 需要改动的文件与工作量
| 文件 | syscall 用途 | 难度 |
|---|---|---|
| `stdio.c` | 0/1/2/3/8/87 | 低 —— 可改用 cg64 已有 builtin |
| `stdlib.c` | 0/2/3/90/231 | 低 |
| `malloc.c` | 9 (mmap) | 中 —— Darwin mmap 签名不同 |
| `sanitizer.c` | 1/9/231 | 中 |
| `thread.c` | 9/11/60/202 + clone | **高 —— 需重写线程层** |

### 推荐路径：优先让 stdio/stdlib 走具名函数
cg64 已经实现了 Darwin 版的 `read`/`write`/`open`/`close` builtin。
若 `stdio.c` 改为调用具名函数而非 `__syscall(1,...)`：
- arm64 自动走 cg64 的正确 Darwin builtin
- x86 侧没有这些 builtin，需要**同时**为 x86 提供，
  或保留 `__syscall` 写法并新增一份 `stdio_darwin.c`

fakecc 的模块系统没有条件编译（无 `#ifdef`），所以只能靠**按目标选文件**。
这与 runtime 现有的单文件结构冲突，是 T16 的主要设计工作量。

### thread.c 需要整体重写
Linux 的 `clone()` + `futex()` + `%fs:0` TLS 在 Darwin 都不存在。
Darwin 的路径是 `pthread_create`（libSystem）或 `bsdthread_create`（syscall 360）。
在没有 libSystem 的 freestanding 前提下，需要自己管理栈、TLS 和同步。
建议：在 T16 之前，arm64 上把 thread.c 整体排除出 runtime 包，
并让 `pthread_create` 返回 ENOSYS —— 线程用例失败，但其他 runtime 功能可用。

## 建议的推进顺序
1. **先做 syscall 号码转译层**：在 cg64 的 `emit_syscall` 里按目标把 Linux
   号码映射到 Darwin 号码。改动小（一个函数），收益最大（stdio/stdlib/malloc
   全部可用），且不触碰 runtime 源码。⚠️ 但要排除 futex/clone 这类无映射的。
2. thread.c 排除出 arm64 runtime 包，`pthread_create` 返回错误。
3. 之后再考虑 thread.c 的 Darwin 重写。

## 实测数据（2026-10-05）
runtime 链接开关现为 `FAKECC_ARM64_RUNTIME=1`（默认关闭，不用改代码重编）。
e2e -O1 演进（-O1，全量 2685）：

| 状态 | 通过 | 说明 |
|---|---|---|
| 关闭（当前 HEAD `1ab31210`） | **2474** | runtime 未链接，printf 等不可用 |
| 开启（只有号码转译） | 1947 | 大量程序被全局指针 bug 拖垮 |
| 开启 + 指针全局修复 | 2366 | — |
| 开启 + 按目标选文件（va_copy 修好） | 2379 | printf/sprintf 已能工作 |
| 开启 + 用户符号覆盖（`0f83802f`） | 2411 | 链接器语义修复 |
| 开启 + 标签/函数地址重定位（`856c9b75`） | 2450 | SIGILL 45→0，仍差 24 |
| 开启 + 折叠偏移随 reloc 走（`f246b8cb`） | **2536** | 🎉 首次超过基线 |

**结论：runtime 门控已默认开启。** `f246b8cb` 修掉 `fold_page_addend` 的偏移
丢失后，开启 runtime 拿到 2536，超过不开启时的 2474。

不开反而更差：不链接 runtime 就没有 printf、malloc、str* 这一整片 libc 替代品。
`FAKECC_ARM64_RUNTIME=0` 保留旧行为（只链接 freestanding 用户代码），用于
定位 runtime 问题时做二分。

### 三个真正的元凶（按发现顺序）
| 提交 | 症状 | 根因 |
|---|---|---|
| `fead10a3` | printf 跳到垃圾地址 | `__fakecc_va_copy` 拷 24 字节，arm64 的 va_list 只有 8 |
| `0f83802f` | `duplicate symbol 'memcpy'` | 链接器不认 C 的"库符号可被替换"，用户定义输给包定义 |
| `856c9b75` | 45 个 SIGILL | `&&label` 和 `&func` 在多 TU 下烘焙了模块内偏移 |
| `f246b8cb` | `a[i]` 读成 `a[0]` | `fold_page_addend` 把偏移折进 gfix，却没随 reloc 传给链接器 |

**共同的形状**：都是"编译期算出的信息在链接期失效"。多 TU 直连时链接器会
拼接各模块的段，任何在 codegen 阶段定下来的偏移或地址都不再成立。

### 失败清单差集分析（2026-10-05 晚）
用 `comm -13` 对比两份清单，结论很清楚：

| | 总失败 | gcc_torture | runtime/ |
|---|---|---|---|
| runtime 关闭 | 215 | 127 | 46 |
| runtime 开启（修符号覆盖前） | 335 | **235** | **28** |

开门**确实有用**（runtime/ 从 46 降到 28），只是被 gcc_torture 的新增抵消。
那 108 个新增全是 `duplicate symbol 'memcpy'/'abs'/'fputs'` ——
gcc torture 用例自己定义这些函数，而 runtime 也有一份。

**C 的语义是库符号可被替换**（用户定义优先），链接器之前一律报重复。
`0f83802f` 修好后：2379 → 2411。

⚠️ 抽样验证 `mmap`/`signal`/`pow`/`longjmp` 这类"新增"失败：
**开与关两种模式都失败**（只是链接期 vs 编译期报错形式不同），**不是回归**。

⚠️ 不要再单独解除门控来"试试看"，每次评估要花 ~25 分钟 e2e。

## 开启时的失败分类（307 条，2026-10-05）
| 类别 | 数量 | 说明 |
|---|---|---|
| `expected 0, got 134`(SIGABRT) | 116 | 多数在 gcc_torture/builtins/，调用 `__builtin_*_chk`，依赖 memcpy/abort 等 libc |
| `expected 0, got 132`(SIGILL) | 38 | 非法指令，多为 aggregates/ 与向量相关 |
| `expected 0, got 1` | 33 | 一般错误返回 |
| 编译期 arm64 backend 错误 | 61 | 其中 vector wider than ×6、aggregate stack blob ×4 |
| 编译器段错误 | 4 | decimal_float / decimal_global_init |

**重要澄清（已验证）**：
1. 抽样 `gcc_torture_builtin_memcpy_chk.c` 在**开与关两种模式下都失败**（都 rc=134），
   说明 SIGABRT/SIGILL 这批**多数与 runtime 无关**。
2. `memcpy` 在**两种模式下都能用**（rc=7）—— cg64 有 `memcpy` builtin，
   不依赖 runtime。所以"开了 runtime 却没有 memcpy"这个猜想被排除。
3. memcpy_chk 用例 abort 的真正原因：该 gcc torture 探针在 x86 上会被死函数
   消除优化掉（`link_error()` 是个未被使用的纯函数），但 arm64 的 cg64
   没做这个优化，于是它真的被调用了 → abort。

**结论：那 116 个 SIGABRT 主要是 cg64 的死函数消除能力缺口，与 runtime 无关。**

## 🔴 下一步（2026-10-05 深夜，已精确定位到一条指令）

**用 `IR_GADDR` 取全局数组元素下标时，写入路径对、读取路径丢索引。**

```c
int a[4];
a[2] = 1;        /* 写入 */
if (a[2] != 1)   /* 读取 —— 这里错 */
```

生成的代码（runtime 开启 → 走 `macho_link_objects`）：

```asm
; 写入 —— 正确
adrp x4, 0x100034000
add  x4, x4, #0
mov  w2, #2
mov  w3, #4
mul  x3, x2, x3        ; x3 = 8
add  x2, x4, x3        ; x2 = &a[2]  ✓
str  w5, [x2]

; 读取 —— 错在下一行
adrp x4, 0x100034000
add  x4, x4, #0
mov  w2, #2
mov  w3, #4
sxtw x2, w2
mul  x3, x2, x3        ; x3 = 8 算出来了
mov  x2, x4            ; ← x3 被丢弃，退化成 &a[0]
ldrsw x2, [x2]         ; 读 a[0]
```

**`mul` 算出的索引在读取路径上没有被加进地址。** 用 `int *p = &a[2]; *p = 7;`
或 `p != &a[2]` 都正确 —— 只有 `a[i]` 形式出错，所以问题在数组下标的 lowering，
不在 `IR_GADDR` 本身。

### 已排除的可能（都验证过）
| 假设 | 验证结果 |
|---|---|
| ADRP 少了 `MACHO_BASE_VA` | ✗ 反汇编里 `adrp x4, 0x100034000` 正是 `__bss` 的 VA |
| `__bss` 与 runtime 变量重叠 | ✗ `bss_base[0]=0`（16 字节），`mod[1]` 从 16 开始 |
| 优化器（mem2reg） | ✗ `-O0` 同样失败 |
| `data`/`bss` 段布局 | ✗ `vmsize=0x4290` 正确覆盖 `__data`+`__bss` |
| 字段偏移丢失 | ✗ `char buf[8]; buf[4]` 正确；`int a;` 正确 |
| 柔性数组成员 | ✗ `int z[4]` 定长也失败 |
| `eval_abs_addr_const` 折叠 | ✗ 那两个函数只被自己递归调用，是死代码 |

### 触发条件（系统测试结果）
| 全局 | 访问 | 结果 |
|---|---|---|
| `int a;` | `a`（偏移 0） | ✓ |
| `int a[4];` | `a[2]` | ✗ |
| `struct {int a;int b;}` | `s.b` | ✗ |
| `struct {int a;int b;int z[4];}` | `s.b` | ✗ |
| `int *p = &a[2]; *p = 7;` | 指针 | ✓ |

**规律：凡是走"基址 + 运行时索引"的寻址都错，纯基址或纯指针都对。**

## 🔴 下一步（2026-10-05 晚，已定位到具体一行但未修完）

**`macho_section_offsets()` 返回文件偏移，而 `macho_link_objects` 用它补 ADRP 立即数。**

`macho_write_exec` 走 dyld chained fixup，要文件偏移 —— 那里没问题。
但链接器补 `adrp` 的 page 立即数时需要的是**虚拟地址**，两者差 `MACHO_BASE_VA`。
少这一层时，每个全局变量/数组的 ADRP 都指向低 4GB 的错误页。

最小复现（单模块正确，多 TU 读回错误的值）：
```c
int arr[4];
int main(void) { arr[2] = 40; return arr[2] == 40 ? 7 : 1; }
```

试过改成 `ro_va/data_va/bss_va = off + MACHO_BASE_VA`：
- 单模块与基线无回归（`arr`/struct/空初始化器都 rc=7）
- runtime 开启时从"读回错值 rc=1"变成 **SIGSEGV**

方向对但有连带问题没查清（很可能是 rebase 记录也吃到了 VA ——
`emit_module_add_rebase` 的 slot 期望文件偏移，加 VA 后会报
`rebase slot 0x100030d18 outside __data`）。

⚠️ **以上判断是错的，已回退。** 两条理由：

1. 反汇编里 `adrp x4, 0x100034000` **正是 `__bss` 的 VA**
   （`otool -l` 确认 `__bss addr = 0x100034000`）—— cg64 烘焙时**已经**
   加了 `MACHO_BASE_VA`，链接器本来就不缺。
2. `patch_adrp(w, pc, tgt)` 的 **pc 和 tgt 必须在同一坐标系** —— 它算的是
   `(tgt - pc) >> 12` 的页差。`pc` 来自 `macho_text_offset()`（文件偏移），
   所以 `tgt` 也必须是文件偏移。只改 `tgt` 会让差值错 4GB。

实测：按上面那条建议改完，baseline 无回归，但 runtime 开启时
从 rc=1 变成 SIGSEGV —— 正是坐标系不一致的表现。

### 坐标系速查（24 小时里在这里栽了三次）
| 量 | 坐标系 | 出处 |
|---|---|---|
| `macho_text_offset()` | 文件偏移 = 1024 | macho.c:345 |
| `macho_section_offsets()` 的三个返回 | 文件偏移 | 用于 dyld chained fixup |
| 段的 `sec.addr` / `vmaddr` | **VA** = `MACHO_BASE_VA + 文件偏移` | macho.c:541 / :529 |
| 反汇编里的 `adrp` 立即数 | **VA** | — |
| `EmitRebase.slot` / `.target` | 文件偏移 | `macho_build_fixups` 用 `data_page` 校验 |

**判据：先拿 `otool -l` 的段 addr 对一下反汇编里的 adrp 目标，再动手。**

## 下一个要查的
1. **开了 runtime 后 `memcpy`/`abort` 是否仍未定义**？runtime/stdlib.c 有它们，
   但若因某种原因没被链接进镜像，就会继续报 undefined。
2. aggregates/ 的 SIGILL（向量宽度、aggregate stack blob 未支持）是独立的
   cg64 能力缺口，与 runtime 无关，但会拖累总数。

## 已查明并修复的前置 bug
1. **syscall 号码**（`109a7a53`）—— 已在 `emit_syscall` 里加 Linux→Darwin 映射。
2. **多 TU 布局烘焙**（`f10186a3`）—— 直接多 TU 链接时，cg64 把单模块的段偏移
   烘焙进 dyld rebase；链接器拼接段后偏移失效，导致 `stdout = &_rt_stdout`
   这类指针全局指向错误位置。修法与 `-c` 一致：走重定位路径。
3. **`__clone` 弱解析**（`4a973536`）—— runtime/thread.c 用 Linux clone。
4. **`.o` 写盘丢失折叠偏移**（`55e1a56b`）—— 与上文"a[2] 读成 a[0]"同根，
   但只出现在 `-c` 写盘再链接的多 TU 路径：`fold_page_addend` 把 `&g+n`
   折进 PAGE21/PAGEOFF12 的 addend，而 `macho_write_object` 逐记录序列化时
   丢弃了 addend —— arm64 Mach-O 要求在该 reloc 前发一条
   ARM64_RELOC_ADDEND(type 10) 记录。读盘侧（`macho_read_object`）本来就
   认 ADDEND，只有写盘缺失。现象：兄弟 TU 里 `wide[1]` 读成 `wide[0]`、
   结构体字段读到其对象基址；`test_cg64_native:1230/:1517` 由此转绿。
   直接可执行路径不经序列化，所以单 TU 一直正常。修复后 fakecc 的 `.o`
   也能被 Apple 原生 `ld` 正确链接（addend 记录对其可见）。

## 下一个要查的
printf 崩溃点 `0x3e02a1303f7`（垃圾地址）说明还有一个指针/地址错误未修。
建议下一步：在多 TU 下打印各模块的段布局与符号地址，与链接器合并后的
`text_base/ro_base/data_base` 对照，定位是哪个地址算错。
可能与 `FILE` 结构体在 `__data` 里的布局、或 `ensure_stdio` 里的
`open_files[64]` 静态数组有关。

## 前置条件（已完成）
- ✅ 多 TU 直接编译能真正链接（`48ac392a`）—— runtime 是"第二个 TU"
- ✅ 允许引用兄弟 TU 的全局/TLS/函数地址（`a4b7d30b`）
- ✅ macho 能跳过 type 10 ADDEND（`a4b7d30b`）
- ✅ `__clone` 弱解析到 0（`4a973536`）
- ✅ BRANCH26_TAIL 在 weak 路径可用（`4a973536`）
- ✅ Linux→Darwin syscall 号码转译（`109a7a53`）
- ✅ 多 TU 不再烘焙单模块段偏移（`f10186a3`）
- ❌ runtime 内部仍有地址 bug（printf 跳垃圾地址）→ **门控必须继续关闭**
