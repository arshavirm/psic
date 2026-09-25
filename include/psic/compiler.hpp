#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <psic/export.hpp>

namespace psic {

// LLVM optimization levels. O0 disables optimization; O2 is the default.
enum class OptimizationLevel { O0, O1, O2, O3, Os, Oz };

// Selects which in-memory result compile() produces.
enum class OutputKind { LLVMIR, Object };

// Severity attached to a diagnostic returned by compile().
enum class DiagnosticSeverity { Error, Warning, Note };

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
};

struct CompileOptions {
    // Name written into the generated LLVM module.
    std::string moduleName = "module";

    // Empty values infer from targetTriple; with no triple they default to
    // x86_64 and Linux. WebAssembly with no OS or triple defaults to freestanding.
    std::string architecture;
    std::string operatingSystem;

    // Optional LLVM target triple. If supplied, explicit architecture/OS values
    // must agree with it.
    std::string targetTriple;

    OptimizationLevel optimization = OptimizationLevel::O2;
    OutputKind output = OutputKind::LLVMIR;
};

struct CompileResult {
    bool success = false;

    // Only the selected output is populated, and only when success is true.
    std::string ir;
    std::vector<std::uint8_t> object;

    // Errors, warnings, and notes produced during compilation.
    std::vector<Diagnostic> diagnostics;
};

// Compile PSI source entirely in memory. No files are opened and diagnostics
// are returned instead of printed. Calls are thread-safe but currently
// serialized. Source/target errors are diagnostics; allocation failures may throw.
PSIC_EXPORT CompileResult compile(const std::string& source, const CompileOptions& options = {});

} // namespace psic
