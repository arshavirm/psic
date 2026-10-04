# PSIC

PSIC compiles PSI for LLVM targets and provides C and C++ APIs for embedding.

## Function pointers

Function names can be passed as `call` arguments or assigned to pointer registers.
Use `void*` for callbacks:

```text
func i32 increment i32 value { i32 next = add value 1; ret next; }
func i32 apply void* callback i32 value {
    i32 result = call callback value;
    ret result;
}
func i32 main {
    i32 result = call apply increment 41;
    ret result;
}
```

Indirect calls use the destination type as their return type, and each argument's
type as its parameter type. These must match the callback's actual signature.
Use explicitly typed registers for indirect-call arguments: integer literals
have type `i64`. A call without a destination assumes a `void` return.
Local registers take precedence over function names.

## Target instructions

On x86 and x86_64, `#int 128;` emits a software interrupt. Its vector must be
an integer literal from 0 to 255. It has no result and acts as a compiler memory
and flags barrier. Register inputs and outputs use PSI's existing special
register mechanism; `#int` does not supply a syscall calling convention.
`#ud2;` emits the x86 undefined instruction. Both are rejected on other targets.
Existing target instructions include x86 fences, ARM barriers and wait/event
instructions, RISC-V `#ecall`, `#ebreak`, and `#fence_i`.

## Embedding and reusable JIT modules

Include `<psic/compiler.hpp>` and link `PSIC::psic`. `compile()` returns IR or
object bytes with diagnostics. `execute()` compiles and invokes a native entry
taking no arguments and returning `void` or `i32`.

`prepareJit()` compiles once without running an entry, and exposes defined
functions with arbitrary native signatures:

```cpp
auto result = psic::prepareJit(
    "func i32 add_values i32 left i32 right { i32 sum = add left right; ret sum; }");
if (result.success) {
    auto address = result.module->functionAddress("add_values");
    auto add = reinterpret_cast<std::int32_t (*)(std::int32_t, std::int32_t)>(address);
    auto sum = add(20, 22);
}
```

Keep `result.module` alive while using its function addresses. Lookup returns
zero for missing names. The caller must use the exact PSI signature and native
ABI. Synchronize calls that share mutable globals. External PSI declarations
require explicit `JitOptions::externalFunctions` mappings; mapped host functions
must remain alive for the module's lifetime. Cross-target JIT is rejected.

The C API in `<psic/c_api.h>` offers `psic_prepare_jit_source()` and
`psic_jit_result_function_address()`. Check `psic_jit_result_success()`, read
diagnostics on failure, and keep the result alive until all calls finish.
Destroy it with `psic_jit_result_destroy()`. Existing compile and execute APIs
remain available; no LLVM types are exposed through the public API.

## CI coverage

GitHub Actions builds and tests eight configurations: Linux x86_64 and ARM64
with LLVM 18/GCC and LLVM 19/Clang, macOS ARM64 and Intel, and Windows x86_64
on Server 2022 and 2025. The LLVM 19 x86_64 build uses a shared library.
Linux x86_64 also runs Debug shared-library and sanitizer builds.
Every configuration runs the C/C++ API, native runtime, installed-package,
CLI, and cross-target object-generation tests. The latter cover x86, ARM,
WebAssembly, RISC-V, PowerPC, MIPS, LoongArch, and SystemZ; they check emitted
objects rather than executing code for those non-native targets.
Failed jobs upload CTest logs, and successful jobs upload installed packages.
