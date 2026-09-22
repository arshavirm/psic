#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef PSIC_EXPORT_HPP
#define PSIC_EXPORT_HPP

// Automatically generated export header for PSIC library
// This header defines the PSIC_EXPORT macro for DLL/shared library visibility

#ifdef PSIC_STATIC_DEFINE
  // Static library - no export/import needed
#  define PSIC_EXPORT
#else
#  ifdef _WIN32
    // Windows DLL import/export
#    ifdef psic_lib_EXPORTS
       // Building the library - export symbols
#      define PSIC_EXPORT __declspec(dllexport)
#    else
       // Using the library - import symbols
#      define PSIC_EXPORT __declspec(dllimport)
#    endif
#  else
    // Unix-like systems - use visibility attribute
#    define PSIC_EXPORT __attribute__((visibility("default")))
#  endif
#endif

// Deprecated macro for marking deprecated API elements
#ifndef PSIC_DEPRECATED
#  ifdef _MSC_VER
#    define PSIC_DEPRECATED __declspec(deprecated)
#  else
#    define PSIC_DEPRECATED __attribute__((__deprecated__))
#  endif
#endif

// No-op macro for future use
#ifndef PSIC_NOEXCEPT
#  define PSIC_NOEXCEPT noexcept
#endif

#endif // PSIC_EXPORT_HPP

namespace psic {

enum class OptimizationLevel { O0, O1, O2, O3, Os, Oz };
enum class OutputKind { LLVMIR, Object };
enum class DiagnosticSeverity { Error, Warning, Note };

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
};

struct CompileOptions {
    std::string moduleName = "module";
    // Empty names select x86_64 Linux, or infer from targetTriple.
    // WebAssembly without an explicit OS defaults to freestanding.
    std::string architecture;
    std::string operatingSystem;
    std::string targetTriple;
    OptimizationLevel optimization = OptimizationLevel::O2;
    OutputKind output = OutputKind::LLVMIR;
};

struct CompileResult {
    bool success = false;
    // Only the requested output is populated, and only on success.
    std::string ir;
    std::vector<std::uint8_t> object;
    std::vector<Diagnostic> diagnostics;
};

// Compiles in memory: no files are opened and diagnostics are not printed.
// Calls are safe from multiple threads; the compiler currently serializes them.
// Source/target errors are returned as diagnostics. Allocation failures may throw.
PSIC_EXPORT CompileResult compile(const std::string& source, const CompileOptions& options = {});

} // namespace psic
