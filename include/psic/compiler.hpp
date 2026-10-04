#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <cstddef>
#include <string_view>
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
    // One-based source coordinates. Zero means the diagnostic is not tied to
    // a single PSI token (for example, a target initialization failure).
    std::size_t line = 0;
    std::size_t column = 0;
    std::string sourceName;
    // Exact source line when the diagnostic has source coordinates.
    std::string sourceLine;
    bool hasSourceLine = false;
};

struct CompileOptions {
    // Name written into the generated LLVM module.
    std::string moduleName = "module";

    // Optional source label copied into diagnostics (for example, a filename
    // supplied by an embedding compiler). It does not affect generated code.
    std::string sourceName;

    // Empty values infer from targetTriple; with no triple they default to
    // x86_64 and Linux. WebAssembly with no OS or triple defaults to freestanding.
    std::string architecture;
    std::string operatingSystem;

    // Optional LLVM target triple. If supplied, explicit architecture/OS values
    // must agree with it.
    std::string targetTriple;

    // Optional LLVM subtarget selection. Empty CPU selects the backend's
    // generic CPU; features are comma-separated LLVM feature overrides.
    std::string targetCPU;
    std::string targetFeatures;

    OptimizationLevel optimization = OptimizationLevel::O2;
    OutputKind output = OutputKind::LLVMIR;
};

struct CompileResult {
    bool success = false;

    // Normalized effective LLVM target triple selected for this compilation,
    // including the default profile when no target selector was supplied.
    std::string targetTriple;

    // Only the selected output is populated, and only when success is true.
    std::string ir;
    std::vector<std::uint8_t> object;

    // Errors, warnings, and notes produced during compilation.
    std::vector<Diagnostic> diagnostics;
};

// Compile PSI bytes entirely in memory without copying the input source. The
// source view must remain valid for the duration of this call. No files are
// opened and diagnostics are returned instead of printed. Independent calls
// may compile concurrently. Source/target errors are diagnostics; allocation
// failures may throw.
PSIC_EXPORT CompileResult compile(std::string_view source,
    const CompileOptions& options = {});

// Compatibility overload for existing source and binary clients.
PSIC_EXPORT CompileResult compile(const std::string& source,
    const CompileOptions& options = {});

// Keep string literals and null-terminated source convenient despite the
// string and string_view overloads. A null pointer is treated as empty source.
inline CompileResult compile(const char* source, const CompileOptions& options = {})
{
    return compile(source ? std::string_view(source) : std::string_view{}, options);
}

// A small PSI source builder. Arguments are caller-supplied PSI syntax fragments;
// the builder assembles declarations and bodies, and compile() validates them.
class PSIC_EXPORT ModuleBuilder {
public:
    explicit ModuleBuilder(std::string name = "module");
    ModuleBuilder& declaration(std::string psiDeclaration);
    ModuleBuilder& function(std::string returnType, std::string name,
        std::string parameters, std::string body);
    ModuleBuilder& entry(std::string name, std::string body);
    const std::string& source() const noexcept;
    CompileResult compile(const CompileOptions& options = {}) const;
private:
    std::string name_;
    std::string source_;
};

struct JitOptions {
    // When all three target selectors are empty, execute() selects the native
    // host triple. Explicit selectors must describe the native execution ABI;
    // CPU overrides must match the detected host CPU (or use "generic"), and
    // enabled feature overrides must be present on the host.
    CompileOptions compile;
    // JIT requests must keep compile.output at OutputKind::LLVMIR; object output
    // is rejected instead of silently ignored.
    std::string entry = "main"; // A no-argument PSI entry/function returning void or i32.
    struct FunctionSymbol {
        std::string name;
        void* address = nullptr;
    };

    // Explicit addresses for PSI-declared external functions in the module.
    // Every external declaration requires a mapping. The host owns these
    // functions and must keep them valid for execute().
    std::vector<FunctionSymbol> externalFunctions;
};
class PSIC_EXPORT JitModule {
public:
    virtual ~JitModule() = default;
    // Zero means the function was not defined. Cast addresses to the exact
    // native signature declared in PSI. Keep the module and host mappings
    // alive throughout every call. Lookup is safe concurrently; execution of
    // functions sharing mutable globals requires caller synchronization.
    virtual std::uintptr_t functionAddress(const std::string& name) const noexcept = 0;
};

struct JitResult {
    bool success = false;
    std::int32_t exitCode = 0;
    std::string targetTriple;
    std::vector<Diagnostic> diagnostics;
};
struct JitModuleResult : JitResult {
    std::shared_ptr<JitModule> module;
};
// Compile once without invoking an entry. options.entry is ignored. Function
// addresses remain valid while module is owned; host mappings must also remain
// alive. Source views only need to remain valid for this call.
PSIC_EXPORT JitModuleResult prepareJit(std::string_view source, const JitOptions& options = {});
// Compile and execute a native PSI entry point in-process. Cross-target JIT
// is rejected. The entry must take no arguments and return void or i32.
// Source, target, and JIT setup failures are returned as diagnostics;
// allocation failures may throw.
PSIC_EXPORT JitResult execute(std::string_view source, const JitOptions& options = {});
PSIC_EXPORT JitResult execute(const std::string& source, const JitOptions& options = {});
inline JitResult execute(const char* source, const JitOptions& options = {})
{
    return execute(source ? std::string_view(source) : std::string_view{}, options);
}

} // namespace psic
