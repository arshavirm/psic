#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <psic/export.hpp>

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
