#include <psic/compiler.hpp>
#include "codegen.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "target.hpp"
#include "logging.hpp"
#include "validation.hpp"
#include <mutex>
#include <stdexcept>

namespace psic {
namespace {
std::mutex compilationMutex;

struct TargetConfig {
    Architecture architecture;
    OperatingSystem operatingSystem;
};

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
    const bool isWasm = target.architecture == Architecture::WASM32
        || target.architecture == Architecture::WASM64;

    if (!hasExplicitOs && !hasTriple && isWasm)
        target.operatingSystem = OperatingSystem::FreeStanding;
    if (hasExplicitOs && hasTriple
        && target.operatingSystem != pickOperatingSystem("", options.targetTriple))
        throw std::runtime_error("OS conflicts with target triple");
    return target;
}

// The logger uses thread-local ambient state. Scope its sink to one public
// compile call, then restore the prior sink for the surrounding caller.
struct DiagnosticScope {
    std::vector<Diagnostic>* previous;
    explicit DiagnosticScope(std::vector<Diagnostic>& diagnostics)
        : previous(psi::setDiagnosticSink(&diagnostics)) { psi::resetErrors(); }
    ~DiagnosticScope() { psi::setDiagnosticSink(previous); psi::resetErrors(); }
};
}

CompileResult compile(const std::string& source, const CompileOptions& options)
{
    // LLVM target initialization is shared across calls, so compilation is serialized.
    std::lock_guard<std::mutex> lock(compilationMutex);
    CompileResult result;
    DiagnosticScope diagnosticScope(result.diagnostics);
    try {
        validateOptions(options);
        const TargetConfig target = resolveTarget(options);

        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        ProgramNode program = parser.parseProgram();
        validateProgram(program);
        std::string ir = psi_codegen::compileProgram(options.moduleName, program,
            target.architecture, target.operatingSystem, options.targetTriple);
        if (psi::hadErrors()) return result;
        std::string error;
        if (options.output == OutputKind::LLVMIR) {
            result.ir = psi_codegen::optimizeIR(ir, options.optimization, error);
        } else {
            if (!psi_codegen::compileToObjectMemory(ir, options.targetTriple, result.object, options.optimization, error)
                && error.empty() && !psi::hadErrors())
                error = "object generation failed";
        }
        if (!error.empty()) psi::logError(error);
        result.success = !psi::hadErrors();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        psi::logError(error.what());
    }
    if (!result.success) {
        result.ir.clear();
        result.object.clear();
    }
    return result;
}
} // namespace psic
