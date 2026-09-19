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
struct DiagnosticScope {
    std::vector<Diagnostic>* previous;
    explicit DiagnosticScope(std::vector<Diagnostic>& diagnostics)
        : previous(psi::setDiagnosticSink(&diagnostics)) { psi::resetErrors(); }
    ~DiagnosticScope() { psi::setDiagnosticSink(previous); psi::resetErrors(); }
};
}

CompileResult compile(const std::string& source, const CompileOptions& options)
{
    std::lock_guard<std::mutex> lock(compilationMutex);
    CompileResult result;
    DiagnosticScope diagnosticScope(result.diagnostics);
    try {
        switch (options.optimization) {
        case OptimizationLevel::O0: case OptimizationLevel::O1: case OptimizationLevel::O2:
        case OptimizationLevel::O3: case OptimizationLevel::Os: case OptimizationLevel::Oz: break;
        default: throw std::runtime_error("invalid optimization level");
        }
        if (options.output != OutputKind::LLVMIR && options.output != OutputKind::Object)
            throw std::runtime_error("invalid output kind");
        Architecture arch = pickArchitecture(options.architecture, options.targetTriple);
        OperatingSystem os = pickOperatingSystem(options.operatingSystem, options.targetTriple);
        if (options.operatingSystem.empty() && options.targetTriple.empty()
            && (arch == Architecture::WASM32 || arch == Architecture::WASM64))
            os = OperatingSystem::FreeStanding;
        if (!options.operatingSystem.empty() && !options.targetTriple.empty()
            && os != pickOperatingSystem("", options.targetTriple))
            throw std::runtime_error("OS conflicts with target triple");

        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        ProgramNode program = parser.parseProgram();
        validateProgram(program);
        std::string ir = psi_codegen::compileProgram(options.moduleName, program, arch, os, options.targetTriple);
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
