#include <psic/compiler.hpp>
#include "codegen.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "target.hpp"
#include "logging.hpp"
#include "validation.hpp"
#include <new>
#include <stdexcept>
#include <utility>

namespace psic {
namespace {
struct TargetConfig {
    Architecture architecture;
    OperatingSystem operatingSystem;
};

bool sourceLineAt(std::string_view source, std::size_t requestedLine,
    std::string& sourceLine)
{
    if (requestedLine == 0) return false;
    std::size_t line = 1;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source[index] != '\n' && source[index] != '\r') continue;
        if (line == requestedLine) {
            sourceLine = source.substr(begin, index - begin);
            return true;
        }
        if (source[index] == '\r' && index + 1 < source.size()
            && source[index + 1] == '\n') ++index;
        ++line;
        begin = index + 1;
    }
    if (line != requestedLine) return false;
    sourceLine = source.substr(begin);
    return true;
}

void validateOptions(const CompileOptions& options)
{
    switch (options.optimization) {
    case OptimizationLevel::O0: case OptimizationLevel::O1: case OptimizationLevel::O2:
    case OptimizationLevel::O3: case OptimizationLevel::Os: case OptimizationLevel::Oz:
        break;
    default:
        throw std::runtime_error("invalid optimization level");
    }
    if (options.output != OutputKind::LLVMIR && options.output != OutputKind::Object)
        throw std::runtime_error("invalid output kind");
}

TargetConfig resolveTarget(const CompileOptions& options)
{
    TargetConfig target {
        pickArchitecture(options.architecture, options.targetTriple),
        pickOperatingSystem(options.operatingSystem, options.targetTriple),
    };
    const bool hasExplicitOs = !options.operatingSystem.empty();
    const bool hasTriple = !options.targetTriple.empty();
    const ArchitectureProfileFamily family = architectureProfileFamily(target.architecture);
    const bool wasm = family == ArchitectureProfileFamily::WebAssembly;

    if (!hasExplicitOs && !hasTriple && wasm)
        target.operatingSystem = OperatingSystem::FreeStanding;
    if (hasExplicitOs && hasTriple
        && target.operatingSystem != pickOperatingSystem("", options.targetTriple))
        throw std::runtime_error("OS conflicts with target triple");

    const bool linuxOrFreestanding = target.operatingSystem == OperatingSystem::Linux
        || target.operatingSystem == OperatingSystem::FreeStanding;
    const bool generalPurpose = family == ArchitectureProfileFamily::GeneralPurpose;
    const bool linuxProfileArchitecture = family == ArchitectureProfileFamily::LinuxOrFreestanding;
    const bool genericLLVMArchitecture = family == ArchitectureProfileFamily::GenericLLVM;

    bool validPair = false;
    if (wasm) {
        validPair = target.operatingSystem == OperatingSystem::FreeStanding
            || target.operatingSystem == OperatingSystem::WASI
            || (hasTriple && target.operatingSystem == OperatingSystem::TargetSpecific);
    } else if (target.operatingSystem == OperatingSystem::TargetSpecific) {
        validPair = hasTriple
            && (generalPurpose || linuxProfileArchitecture || genericLLVMArchitecture);
    } else if (genericLLVMArchitecture) {
        validPair = hasTriple && target.operatingSystem != OperatingSystem::WASI;
    } else {
        validPair = target.operatingSystem != OperatingSystem::WASI
            && (generalPurpose || (linuxProfileArchitecture && linuxOrFreestanding));
    }
    if (!validPair) {
        std::string selectedProfile = options.targetTriple;
        if (selectedProfile.empty()) {
            selectedProfile = options.architecture.empty() ? "default architecture" : options.architecture;
            if (!options.operatingSystem.empty())
                selectedProfile += "/" + options.operatingSystem;
        }
        throw std::runtime_error("unsupported architecture/OS profile combination '"
            + selectedProfile + "'");
    }
    return target;
}

bool validateJitEntrySignature(std::string_view source, const std::string& entry,
    std::string& error, SourceLocation& errorLocation)
{
    Lexer lexer(source);
    Parser parser(lexer.tokenize());
    ProgramNode program = parser.parseProgram();
    for (const DeclarationNode& declaration : program.declarations) {
        if (declaration.kind == DeclKind::Entry && declaration.entryDecl.name == entry)
            return true;
        if (declaration.kind != DeclKind::Func || declaration.funcDecl.name != entry)
            continue;

        errorLocation = declaration.location;
        const FuncDeclNode& function = declaration.funcDecl;
        if (!function.hasBody) {
            error = "JIT entry '" + entry + "' must have a definition";
            return false;
        }
        if (!function.args.empty()) {
            error = "JIT entry must take no arguments";
            return false;
        }
        const TypeNode& result = function.returnType;
        if (result.pointerLevel != 0 || result.isView
            || (result.baseName != "void" && result.baseName != "i32")) {
            error = "JIT entry function must return void or i32";
            return false;
        }
        return true;
    }
    // Let the LLVM entry lookup produce the established not-found diagnostic.
    return true;
}
}

CompileResult compile(std::string_view source, const CompileOptions& options)
{
    CompileResult result;
    psi::DiagnosticCaptureScope diagnosticScope(result.diagnostics);
    try {
        validateOptions(options);
        const TargetConfig target = resolveTarget(options);
        result.targetTriple = normalizeTargetTriple(options.targetTriple.empty()
            ? psi_codegen::defaultTriple(target.architecture, target.operatingSystem)
            : options.targetTriple);

        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        ProgramNode program = parser.parseProgram();
        const PointerModel pointerModel = psi_codegen::targetPointerModel(target.architecture,
            target.operatingSystem, result.targetTriple, options.targetCPU, options.targetFeatures);
        validateProgram(program, pointerModel);
        std::string ir = psi_codegen::compileProgram(options.moduleName, program,
            target.architecture, target.operatingSystem, result.targetTriple,
            options.targetCPU, options.targetFeatures);
        if (diagnosticScope.hasErrors()) {
            for (auto& diagnostic : result.diagnostics) {
                diagnostic.sourceName = options.sourceName;
                diagnostic.hasSourceLine = sourceLineAt(source, diagnostic.line,
                    diagnostic.sourceLine);
            }
            return result;
        }
        std::string error;
        if (options.output == OutputKind::LLVMIR) {
            result.ir = psi_codegen::optimizeIR(ir, options.optimization, error,
                options.targetCPU, options.targetFeatures);
        } else {
            if (!psi_codegen::compileToObjectMemory(ir, result.targetTriple,
                    result.object, options.optimization, error,
                    options.targetCPU, options.targetFeatures)
                && error.empty() && !diagnosticScope.hasErrors())
                error = "object generation failed";
        }
        if (!error.empty()) psi::logError(error);
        result.success = !diagnosticScope.hasErrors();
    } catch (const LocatedValidationError& error) {
        psi::logError(error.what(), error.location.line, error.location.column);
    } catch (const LocatedSourceError& error) {
        psi::logError(error.what(), error.location.line, error.location.column);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        psi::logError(error.what());
    }
    if (!result.success) {
        result.ir.clear();
        result.object.clear();
    }
    for (auto& diagnostic : result.diagnostics) {
        diagnostic.sourceName = options.sourceName;
        diagnostic.hasSourceLine = sourceLineAt(source, diagnostic.line,
            diagnostic.sourceLine);
    }
    return result;
}

CompileResult compile(const std::string& source, const CompileOptions& options)
{
    return compile(std::string_view(source.data(), source.size()), options);
}

ModuleBuilder::ModuleBuilder(std::string name) : name_(std::move(name)) {}
ModuleBuilder& ModuleBuilder::declaration(std::string text)
{
    source_ += std::move(text);
    if (source_.empty() || source_.back() != '\n') source_ += '\n';
    return *this;
}
ModuleBuilder& ModuleBuilder::function(std::string result, std::string name,
    std::string parameters, std::string body)
{
    source_ += "func " + result + " " + name;
    if (!parameters.empty()) source_ += " " + parameters;
    source_ += " {\n" + body + "\n}\n";
    return *this;
}
ModuleBuilder& ModuleBuilder::entry(std::string name, std::string body)
{
    source_ += "entry " + name + " {\n" + body + "\n}\n";
    return *this;
}
const std::string& ModuleBuilder::source() const noexcept { return source_; }
CompileResult ModuleBuilder::compile(const CompileOptions& options) const
{
    CompileOptions configured = options;
    if (configured.moduleName == "module") configured.moduleName = name_;
    return psic::compile(source_, configured);
}

static JitModuleResult buildJit(std::string_view source, const JitOptions& options, bool prepare)
{
    JitModuleResult result;
    if (options.compile.output != OutputKind::LLVMIR) {
        result.diagnostics.push_back({DiagnosticSeverity::Error,
            "JIT execution cannot request object-file output", 0, 0,
            options.compile.sourceName, {}, false});
        return result;
    }
    CompileOptions configured = options.compile;
    configured.output = OutputKind::LLVMIR;
    if (configured.architecture.empty() && configured.operatingSystem.empty()
        && configured.targetTriple.empty())
        configured.targetTriple = psi_codegen::nativeTargetTriple();
    if (configured.targetTriple.empty()) {
        try {
            const TargetConfig target = resolveTarget(configured);
            configured.targetTriple = psi_codegen::defaultTriple(
                target.architecture, target.operatingSystem);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            // Let compile() return the canonical target-selection diagnostic.
        }
    }
    if (!configured.targetTriple.empty()) {
        std::string subtargetError;
        if (!psi_codegen::validateJitSubtarget(configured.targetTriple,
                configured.targetCPU, configured.targetFeatures, subtargetError)) {
            result.targetTriple = configured.targetTriple;
            result.diagnostics.push_back({DiagnosticSeverity::Error,
                std::move(subtargetError), 0, 0, configured.sourceName, {}, false});
            return result;
        }
    }
    auto compiled = psic::compile(source, configured);
    result.targetTriple = compiled.targetTriple;
    result.diagnostics = std::move(compiled.diagnostics);
    if (!compiled.success) return result;
    std::string error;
    try {
        SourceLocation errorLocation;
        if (!prepare && !validateJitEntrySignature(source, options.entry, error, errorLocation)) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, std::move(error),
                errorLocation.line, errorLocation.column,
                options.compile.sourceName, {}, false});
            result.diagnostics.back().hasSourceLine = sourceLineAt(source,
                errorLocation.line, result.diagnostics.back().sourceLine);
            return result;
        }
    } catch (const std::exception& parseError) {
        // Compilation already succeeded, so parsing should be deterministic.
        // Keep this boundary diagnostic-based if a future parser change breaks
        // that assumption rather than allowing an exception across the API.
        result.diagnostics.push_back({DiagnosticSeverity::Error,
            std::string("JIT entry validation failed: ") + parseError.what(), 0, 0,
            options.compile.sourceName, {}, false});
        return result;
    }
    try {
        if (!psi_codegen::executeJit(compiled.ir, options.entry, options.externalFunctions,
                result.exitCode, error, options.compile.targetCPU,
                options.compile.targetFeatures, prepare ? &result.module : nullptr)) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, std::move(error), 0, 0,
                options.compile.sourceName, {}, false});
            return result;
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& jitError) {
        result.diagnostics.push_back({DiagnosticSeverity::Error,
            std::string("JIT setup failed: ") + jitError.what(), 0, 0,
            options.compile.sourceName, {}, false});
        return result;
    } catch (...) {
        result.diagnostics.push_back({DiagnosticSeverity::Error,
            "JIT setup failed with an unknown exception", 0, 0,
            options.compile.sourceName, {}, false});
        return result;
    }
    result.success = true;
    return result;
}

JitModuleResult prepareJit(std::string_view source, const JitOptions& options)
{
    return buildJit(source, options, true);
}

JitResult execute(std::string_view source, const JitOptions& options)
{
    return buildJit(source, options, false);
}

JitResult execute(const std::string& source, const JitOptions& options)
{
    return execute(std::string_view(source.data(), source.size()), options);
}
} // namespace psic
