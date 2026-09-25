# Testing PSIC

PSIC uses CTest to run its C++ and Python integration checks. Configure from the
repository root and build before running the suite:

```sh
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
```

The LLVM installation needs every backend exercised by the architecture suite.
Python 3 is needed when tests are enabled. Configure with
`-DBUILD_TESTING=OFF` to skip tests, or `-DPSIC_BUILD_TESTS=OFF` to skip PSIC's
test targets when it is embedded with `add_subdirectory`.

## Test map

| CTest test | Source | What it checks |
| --- | --- | --- |
| `library` | `tests/library.cpp` | In-memory API results, diagnostic capture, error recovery, optimization levels, and concurrent callers. |
| `architectures` | `tests/architectures.py` | Target-specific object headers, instruction/register support, target inference, and invalid target combinations. |
| `cli` | `tests/cli.py` | Argument handling, stdin/stdout, paths, diagnostics, and preservation of output after compilation errors. |
| `runtime_O0`, `runtime_O2` | `tests/runtime.psi`, `tests/runtime.cpp`, `tests/emit_runtime.cpp` | Compiles PSI to a host object, links it with C++, then checks the program's results without and with optimization. |
| `installed_package` | `tests/package.py`, `tests/package/` | Installs and relocates PSIC, builds an external CMake consumer, runs the installed CLI when present, and tests `add_subdirectory` embedding. |

The runtime and package tests are omitted when cross-compiling because they run
native executables. The architecture and CLI tests require the `psic` executable,
so they are omitted when `PSIC_BUILD_CLI=OFF`. The library test only needs the
library target.

## Useful build options

- `-DPSIC_BUILD_CLI=OFF` builds only the library; CLI integration checks are
  consequently unavailable.
- `-DBUILD_SHARED_LIBS=ON` builds a shared library. The package test also checks
  that the installed CLI can find the library after the installation is moved.
- `-DPSIC_SANITIZE=ON` adds AddressSanitizer and UndefinedBehaviorSanitizer flags
  for GCC or Clang builds.

See the root README for language examples, supported targets, and the compiler
pipeline overview.
