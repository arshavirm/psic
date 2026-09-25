# PSIC

PSIC compiles PSI unified assembly to LLVM IR or a relocatable object file.
It requires C++17 and LLVM 18 or newer, with the desired target backends built in.

## How compilation works

The compiler has two entry points: the `psic` command-line program and the
`psic::compile` C++ library API. The CLI handles arguments and file I/O; the
library compiles source in memory and returns output plus structured diagnostics.

For the detailed front-end, lowering, ownership, and output flow, see the
[compiler design note](docs/compiler-design.md).

Every compile follows this path:

1. **Choose a target.** Explicit architecture and OS names are normalized, or
   inferred from a target triple. Conflicting settings and unsupported targets
   become diagnostics before parsing.
2. **Tokenize and parse.** `src/lexer.cpp` turns source text into tokens with
   line/column locations. `src/parser.cpp` builds the program AST in `src/ast.hpp`.
   The program owns parsed values in an arena because AST nodes refer to them by
   pointer; this keeps values alive while AST declarations are copied for lowering.
3. **Validate.** `src/validation.cpp` checks declarations, types, command operands,
   labels, and by-value struct cycles before LLVM IR is built.
4. **Lower to LLVM IR.** `src/codegen_module.cpp` creates types, declarations,
   globals, and function bodies. `src/codegen_instructions.cpp` lowers values and
   commands; `src/codegen_registers.cpp` handles target-specific registers and
   inline assembly. These stages share per-compilation LLVM and symbol state from
   `src/codegen_state.hpp`.
5. **Optimize or emit an object.** `src/codegen.cpp` parses the generated IR,
   applies the selected LLVM optimization pipeline, and either returns optimized
   IR text or emits target-specific object bytes.

Diagnostics flow through `src/logging.cpp`. The library temporarily directs them
into `CompileResult::diagnostics` and restores the previous logger afterward.
The logger's sink and error count are thread-local; compilation calls are
serialized around the compiler core. LLVM target components are registered once
before target-machine construction. The CLI then writes the requested output file;
file handling is not part of the library API.

### Where to look

| Concern | Source |
| --- | --- |
| Public embedding API and result types | `include/psic/compiler.hpp` |
| CLI options, input and output files | `src/main.cpp` |
| Architecture and OS selection | `src/target.cpp` |
| Tokens and lexical rules | `src/lexer.hpp`, `src/lexer.cpp` |
| Grammar and AST construction | `src/parser.hpp`, `src/parser.cpp`, `src/ast.hpp` |
| Pre-codegen checks | `src/validation.cpp` |
| LLVM lowering | `src/codegen_module.cpp`, `src/codegen_instructions.cpp`, `src/codegen_registers.cpp` |
| Optimization and object emission | `src/codegen.cpp` |

```sh
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
```

The integration suite uses Python 3 and all architecture backends listed below.
Configure with `-DBUILD_TESTING=OFF` to build without Python or the cross-target suite.
You can also run it directly: `python3 tests/architectures.py build/psic`.

```sh
build/psic --arch wasm32 --os wasi hello.psi -o hello.o
build/psic --target riscv64-unknown-linux-gnu hello.psi -o hello.o
build/psic --arch aarch64 --emit-llvm hello.psi -o hello.ll
```

## Embedding PSIC

The `psic_lib` library target is available as `PSIC::psic`. It is static by default;
use `-DBUILD_SHARED_LIBS=ON` for a shared library. `-DPSIC_BUILD_CLI=OFF` builds
without the command-line executable. The public C++17 API is
`<psic/compiler.hpp>`; consumers do not include LLVM or internal parser headers.

You can use `add_subdirectory(path/to/psic)` and link `PSIC::psic`, or install it.
PSIC tests default to off when embedded as a subdirectory; set `PSIC_BUILD_TESTS=ON`
and enable CTest to opt in.

```sh
cmake --install build --prefix /path/to/install
```

A consumer project can then use:

```cmake
cmake_minimum_required(VERSION 3.20)
project(example LANGUAGES C CXX)
find_package(PSIC 0.1 CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE PSIC::psic)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/path/to/install`. Static-library
consumers also need the matching LLVM development package (set `LLVM_DIR` if
needed); LLVM's configuration checks use both C and C++. A shared-library consumer
does not need LLVM headers, but the deployed application needs the PSIC and LLVM
runtime libraries. Windows callers must put the installed `bin` directory on their
DLL search path. Installed packages can be relocated together with their headers
and libraries; LLVM remains an external dependency.

```cpp
#include <psic/compiler.hpp>
#include <iostream>

int main() {
    psic::CompileOptions options;
    options.architecture = "wasm32";
    options.optimization = psic::OptimizationLevel::O2;
    options.output = psic::OutputKind::LLVMIR;

    auto result = psic::compile("func i32 answer { ret 42; }", options);
    if (!result.success) {
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.message << '\n';
        return 1;
    }
    std::cout << result.ir;
}
```

Set `OutputKind::Object` to receive relocatable object bytes in `result.object`.
The library does not open files or print diagnostics. Only the selected output is
populated on success; failures leave both output fields empty and return structured
error/warning/note diagnostics. `CompileOptions` also accepts `moduleName`,
`operatingSystem`, and `targetTriple`, with the same target rules as the CLI.
Optimization applies to both LLVM IR and object output, including CLI `--emit-llvm`.
Allocation failures can propagate as C++ exceptions; ordinary source/target errors
are returned as diagnostics.

Repeated calls own and release their parser data. Calls from multiple threads are
safe but currently serialized around the compiler core. The logger's diagnostic
sink is thread-local and scoped to each call. The public API does not expose the
core's mutable AST or LLVM state.

## Compiler checks

For details on what each CTest target covers, test prerequisites, and CMake test
options, see the [testing guide](docs/testing.md).

CTest covers:

- Library API output, diagnostics, repeated-call recovery, all optimization levels,
  and concurrent callers.
- 381 cross-target cases: object format/width/byte order, special instructions and
  registers, target inference, and invalid combinations.
- Native executable results at `-O0` and `-O2`: arithmetic, signed/unsigned shifts,
  loops, arrays, structs, pointer loads/stores, globals, floating point, intrinsics,
  and calls into host C++ code.
- CLI arguments, stdin/stdout, output paths, diagnostic locations, and preservation
  of existing output files after compiler errors.
- Separate CMake consumers that link and run using a relocated installation and
  `add_subdirectory`, with library-only embedding.

The compiler validates operand counts, label/name requirements, result targets,
void types, alignment, duplicate symbols/fields/arguments/labels, and by-value
struct cycles before lowering. It also diagnoses invalid load/store and integer
operation types before constructing LLVM instructions. This avoids crashes for
those malformed inputs and keeps subsequent library calls usable. Symbol names
must be unique: declare external functions once, and define local functions once
without a separate prototype.

Use `-DPSIC_SANITIZE=ON` with GCC/Clang to instrument the library and its callers
with address/undefined-behavior sanitizers. Native execution and installed-consumer
tests are disabled when CMake is cross-compiling. The architecture suite still
validates foreign objects without executing them.

Objects require a target-compatible linker and runtime. WASM objects are
relocatable `.o` files, not standalone executable modules. WASI imports can be
declared as external PSI functions and resolved during linking.

## Targets

| `--arch` | Width / byte order | Default OS | Other supported OS |
| --- | --- | --- | --- |
| `x86` (`i386`…`i686`) | 32 / little | Linux | none, Darwin, Windows |
| `x86_64` (`x86-64`, `amd64`) | 64 / little | Linux | none, Darwin, Windows |
| `arm` (`armv7`, `armv7a`, `thumbv7`, `thumbv7a`) | 32 / little | Linux | none, Darwin, Windows |
| `aarch64` (`arm64`) | 64 / little | Linux | none, Darwin, Windows |
| `wasm32` (`wasm`) | 32 / little | none | WASI |
| `wasm64` | 64 / little | none | WASI |
| `riscv32`, `riscv64` | 32, 64 / little | Linux | none |
| `ppc` (`powerpc`) | 32 / big | Linux | none |
| `ppc64` (`powerpc64`) | 64 / big | Linux | none |
| `ppc64le` (`powerpc64le`) | 64 / little | Linux | none |
| `mips`, `mips64` | 32, 64 / big | Linux | none |
| `mipsel`, `mips64el` | 32, 64 / little | Linux | none |
| `loongarch64` | 64 / little | Linux | none |
| `s390x` (`systemz`) | 64 / big | Linux | none |

With no target options, PSIC defaults to x86_64 Linux. `--target` selects the
architecture and OS before code generation; its exact spelling is preserved in
emitted IR. Conflicting `--arch` or `--os` options are rejected. The target's LLVM
data layout is applied before generating IR, including pointer width and byte order.
An LLVM build missing a selected backend produces an error.

Baseline native register support assumes SSE2 on x86, VFPv3 on ARM32, and F/D on
LoongArch64. These requirements are preserved in emitted IR's function attributes.
RISC-V defaults to LLVM's generic integer ISA; native floating-register extensions
are not exposed. Portable FP and vector operations may be scalarized or lowered to
runtime helpers on targets lacking corresponding hardware. WASM64 requires a
memory64-capable linker/runtime. Selecting WASI does not supply a WASI sysroot.

## Instructions

PSI values are named virtual registers. Architecture-specific or low-level
operations use `#`; physical/special register accesses use `%`. Operands are
separated by spaces and commands end in semicolons.

### Program structure

A source file contains declarations. A function declaration starts with `func`,
then its return type and name, followed by zero or more type/name argument pairs.
End a declaration with `;` to declare an external function; add a block to define
the function. An `entry` declaration defines a named void function. Struct fields
and function arguments are written as adjacent type/name pairs, without commas.
Globals and constants have an initializer:

```text
struct Pair { i32 left i32 right }
func i32 add_pair i32 a i32 b {
    i32 sum = add a b;
    ret sum;
}
func i32 host_value;
i32 shared = 7;
const i32 answer = 42;
entry main {
    i32 value = call add_pair shared answer;
    ret;
}
```

Types are primitive names such as `i32`, `u64`, `f32`, `bool`, and `void`, or a
declared struct name. Append `*` for each pointer level; an optional `:N` after
the type sets alignment (N must be a positive power of two). A command may
declare a typed local (`i32 value;`), assign a value (`value = 4;`), or assign an
instruction result (`i32 sum = add left right;`). Control flow uses `label`,
`jmp`, and `cjmp`; function bodies end with `ret` when a value is returned.

`ref value` produces a pointer to the selected value; applying it to a pointer
produces a pointer-to-pointer. Pointer/integer conversions are never implicit.
Use `#ptrcast` for an explicit pointer reinterpretation, `#ptrtoint` to obtain
an integer address, or `#inttoptr` to reconstruct a pointer. These casts require
an explicitly typed result. Pointer/integer conversions require the integer
width to equal the target's pointer representation width and are not portable
across targets with non-integral pointers. `#ptrcast` preserves the address but
does not make accesses through the new type safe. A backend must retain declared
PSI types rather than infer pointee types from its machine IR representation.
Implicit pointer conversion is limited to `null` or an exactly matching base
type and pointer depth. Use `#ptrcast` for other pointer reinterpretations;
accessors and typed `load`/`store` must agree with the pointer's pointee type.

### Primary instruction contract

Instructions without `#` are the portable core. Their meanings come from PSI
types, not the selected processor; a backend may use native instructions or
software lowering but must preserve these results:

- Integer `add`, `sub`, and `mul` wrap modulo the operand width. Integer
  `div`/`mod` trap on a zero divisor and on signed minimum divided by `-1`.
- Integer right shift is arithmetic for signed types and logical for unsigned
  types. Either shift traps when the shift count is negative or at least the
  operand width; counts are not silently masked by hardware.
- For two numeric operands, the right operand converts to the left operand's
  type before the operation. Integer widening follows the left operand's
  signedness; integer/floating mixing is rejected. The left operand determines
  operation width and signedness.
- Integer comparisons are signed or unsigned according to the operand type.
  Floating comparisons are ordered: comparisons involving NaN, including
  `neq`, return false. `land`, `lor`, and `lnot` return normalized `bool`
  values; `land`/`lor` are value operations, not short-circuit control flow.

Division traps and out-of-range shifts are explicit PSI behavior, not accidental
LLVM or CPU behavior. `#` operations naming a device service, ABI, register, or
native instruction remain target-specific; use them only on documented targets.
Their absence on another target does not alter the portable core.

```text
entry main {
    u32 pages = #memory.size;
    u32 previous = #memory.grow 1;
    u32 current = %memory_pages;
    ret;
}
```

Special instruction names can use dots or underscores (`#memory.size` equals
`#memory_size`, `#fence.i` equals `#fence_i`). Names are case-sensitive.

The existing portable operations remain available across targets:

- `#clz`, `#ctz`, `#popcnt`, `#bswap`: one integer operand.
- `#fadd`, `#fsub`, `#fmul`, `#fdiv`: two matching `f32` or `f64` operands,
  lowered through LLVM on every architecture.
- `#vadd`, `#vsub`, `#vmul`, `#vdiv`, `#vdivi`, `#vdivu`, `#vand`, `#vor`,
  `#vxor`, `#vmin`, `#vmax`, `#vfma`, `#vsplat`: existing vector operations.
- `#atomicload`, `#atomicstore`, `#cas`, and the existing atomic RMW operations:
  `#atomicadd`, `#atomicsub`, `#atomicand`, `#atomicor`, `#atomicxor`,
  `#atomicnand`, `#atomicxchg`, `#atomicmax`, `#atomicmin`, `#atomicumax`,
  `#atomicumin`, `#atomicfadd`, `#atomicfsub`.
- `#fence`: sequentially consistent LLVM fence; `#trap`: LLVM trap.
- `#nop`: no operands; emits native NOP, or no operation on WASM.

LLVM and the selected ISA determine whether atomic/vector operations need target
extensions or runtime helpers. This compiler does not provide those helpers.

| Target | Additional instructions |
| --- | --- |
| WASM32/64 | `#memory_size` (no operands), `#memory_grow pages` |
| x86 / x86_64 | `#pause`, `#lfence`, `#sfence`, `#mfence`; `#rdtsc` returns `u64` |
| ARM32 / AArch64 | `#yield`, `#dmb`, `#dsb`, `#isb`, `#wfi`, `#wfe`, `#sev` |
| AArch64 | `#sevl` |
| RISC-V | `#ecall`, `#ebreak`, `#wfi`, `#fence_i` |
| PowerPC | `#sync`, `#lwsync`, `#isync`, `#eieio` |
| MIPS | `#sync` |
| LoongArch64 | `#dbar`, `#ibar` |
| SystemZ | `#serialize` (`bcr 15,0`) |

All native operations in this table take no operands except WASM memory growth.
ARM barriers use the `sy` domain; LoongArch barriers use hint 0. Memory barriers
also constrain compiler reordering. Privileged operations require the appropriate
execution environment; counters and optional instructions require ISA/runtime
support (for example RISC-V Zicntr and Zifencei).

WASM memory operations address memory 0. Sizes and growth amounts are in 64-KiB
pages, with `u32` results on WASM32 and `u64` on WASM64. Growth returns the previous
size, or the all-ones value on failure. Integer growth operands are zero-extended
or truncated to the target address width. `%memory_pages` performs a fresh size
query; it is read-only. WASM has no physical CPU register bank, so names such as
`%rax`, `%sp`, and `%local0` are rejected. PSI variables represent WASM values.

`#syscall number [arg0 ... arg5]` implements the Linux ABI on x86, x86_64,
ARM32, AArch64, and RISC-V. It returns a target-width integer. Other OS/architecture
combinations are rejected rather than using an incorrect ABI. Raw `#ecall` is a
separate operand-free instruction; use `#syscall` for Linux argument placement.

## Special registers

```text
entry main {
    u64 count = %cycle;  // RISC-V: target-width counter read
    ret;
}
```

Registers are target-specific and case-sensitive. Integer writes are zero-extended
or truncated to the register width. Floating registers require their documented
floating type. Unknown registers and writes to read-only values are diagnosed.

| Target | Registers |
| --- | --- |
| x86_64 | Existing `rax`…`r15`, corresponding 32/16/8-bit names; `xmm0`…`xmm15` as `f32`; `mxcsr` as `u32` |
| x86 | Existing `eax`…`esp`, 16-bit names, `al`, `bl`, `cl`, `dl`; `xmm0`…`xmm7` as `f32`; `mxcsr` as `u32` |
| AArch64 | `x0`…`x30` (`u64`), `w0`…`w30` (`u32`), `sp`, `fp`, `lr`; `s0`…`s31` (`f32`), `d0`…`d31` (`f64`) |
| AArch64 system | `nzcv`, `fpcr`, `fpsr`, `tpidr_el0` (`u64`, read/write); `cntvct_el0`, `cntfrq_el0` (`u64`, read-only) |
| ARM32 | `r0`…`r12`, `sp`/`r13`, `lr`/`r14`, `fp`; `s0`…`s31` (`f32`), `d0`…`d15` (`f64`); `apsr` (`u32`, writes NZCVQ flags) |
| RISC-V | `x0`…`x31` and ABI aliases `zero`, `ra`, `sp`, `gp`, `tp`, `t0`…`t6`, `s0`…`s11`, `a0`…`a7`, `fp`; target-width integers; `zero`/`x0` read-only |
| RISC-V counters | `cycle`, `time`, `instret` (target-width, read-only); RV32 also `cycleh`, `timeh`, `instreth` for high halves |
| PowerPC | `r0`…`r31` (target-width), `sp` = `r1`; `f0`…`f31` (`f64`); `lr`, `ctr`, `xer` via dedicated read/write instructions |
| MIPS | `r0`…`r31` and ABI aliases; `hi`, `lo` via dedicated read/write instructions; target-width integers; `zero`/`r0` read-only |
| LoongArch64 | `r0`…`r31`, `zero`, `ra`, `tp`, `sp`, `fp`, `a0`…`a7` (`u64`); `zero`/`r0` read-only |
| SystemZ | `r0`…`r15` (`u64`), `sp` = `r15`, `f0`…`f15` (`f64`) |
| WASM | `memory_pages` (target-width, read-only pseudo-register) |

MIPS32 uses O32 aliases: `a0`…`a3` = r4…r7, `t0`…`t7` = r8…r15.
MIPS64 uses N64 aliases: `a0`…`a7` = r4…r11, `t0`…`t3` = r12…r15.
Both expose `zero`, `at`, `v0`, `v1`, `s0`…`s7`, `t8`, `t9`, `k0`, `k1`, `gp`,
`sp`, `fp`, and `ra`. HI/LO require a pre-Release-6 MIPS ISA.

Physical general-register access retains PSIC's LLVM fixed-register constraint
semantics: it captures or supplies a register at that compiler point. It does not
reserve the register across subsequent PSI statements. LLVM can use registers
between accesses; writes to stack/link/frame registers can violate the function
ABI. Use PSI virtual values for ordinary computation and instructions with explicit
operands for ABI-sensitive operations. System registers use actual reads/writes;
MXCSR uses LLVM's load/store intrinsics. Changes to FP control state do not enable
LLVM strict/constrained floating-point semantics.

The implementation follows LLVM's target triples, inline-assembly constraints,
and `IntrinsicsWebAssembly.td` memory intrinsics. The integration tests compile
real objects, check their machine type/byte order, check WASM headers and intrinsic
widths, and exercise invalid target, operand, and register combinations. They do
not execute cross-architecture machine code or link runtime libraries.
