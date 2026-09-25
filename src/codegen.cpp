#include "codegen_state.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <memory>
#include <mutex>
#include <string>

namespace psi_codegen {

#if LLVM_VERSION_MAJOR >= 19
static void captureLLVMDiagnostic(const llvm::DiagnosticInfo* diagnostic, void*)
#else
static void captureLLVMDiagnostic(const llvm::DiagnosticInfo& diagnostic, void*)
#endif
{
#if LLVM_VERSION_MAJOR >= 19
    const auto& info = *diagnostic;
#else
    const auto& info = diagnostic;
#endif
    std::string message;
    llvm::raw_string_ostream stream(message);
    llvm::DiagnosticPrinterRawOStream printer(stream);
    info.print(printer);
    if (info.getSeverity() == llvm::DS_Error) psi::logError(message);
    else if (info.getSeverity() == llvm::DS_Warning) psi::logWarning(message);
    else if (info.getSeverity() == llvm::DS_Note) psi::logNote(message);
}

void setCodegenDiagnosticHandler(llvm::LLVMContext& context)
{
    context.setDiagnosticHandlerCallBack(captureLLVMDiagnostic, nullptr, true);
}

namespace {

void initializeAllTargetComponents()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmParsers();
        llvm::InitializeAllAsmPrinters();
    });
}

} // namespace

std::unique_ptr<llvm::TargetMachine> buildTargetMachine(const std::string& triple, std::string& errorMessage)
{
    initializeAllTargetComponents();

    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupError);
    if (!target) {
        errorMessage = "failed to look up target '" + triple + "': " + lookupError;
        return nullptr;
    }

    llvm::TargetOptions options;
    auto relocModel = llvm::Reloc::PIC_;
    std::string features;
    const llvm::Triple parsedTriple(triple);
    if (parsedTriple.getArch() == llvm::Triple::x86) features = "+sse2";
    if (parsedTriple.getArch() == llvm::Triple::arm || parsedTriple.getArch() == llvm::Triple::thumb)
        features = "+vfp3";
    if (parsedTriple.getArch() == llvm::Triple::loongarch64) features = "+f,+d";
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(triple, "generic", features, options, relocModel));

    if (!targetMachine) {
        errorMessage = "failed to create a target machine for '" + triple + "'";
    }
    return targetMachine;
}

namespace {

// Shared scaffolding for optimizeIR/compileToObjectMemory: parse IR text into
// a fresh context with the diagnostic handler installed.
std::unique_ptr<llvm::Module> parseIRModule(
    const std::string& irCode, llvm::LLVMContext& context, std::string& errorMessage)
{
    setCodegenDiagnosticHandler(context);
    llvm::SMDiagnostic diagnostic;

    std::unique_ptr<llvm::MemoryBuffer> irBuffer = llvm::MemoryBuffer::getMemBuffer(irCode, "module");
    std::unique_ptr<llvm::Module> module = llvm::parseIR(irBuffer->getMemBufferRef(), diagnostic, context);

    if (!module) {
        std::string diagText;
        llvm::raw_string_ostream diagStream(diagText);
        diagnostic.print("codegen", diagStream);
        diagStream.flush();
        errorMessage = "failed to parse IR: " + diagText;
        return nullptr;
    }
    return module;
}

std::unique_ptr<llvm::TargetMachine> prepareModuleTarget(
    llvm::Module& module, std::string triple, std::string& errorMessage)
{
    if (triple.empty()) triple = module.getTargetTriple();
    if (triple.empty()) triple = llvm::sys::getDefaultTargetTriple();

    std::unique_ptr<llvm::TargetMachine> targetMachine = buildTargetMachine(triple, errorMessage);
    if (!targetMachine) return nullptr;

    module.setTargetTriple(triple);
    module.setDataLayout(targetMachine->createDataLayout());
    return targetMachine;
}

void runOptimizationPipeline(llvm::Module& module, llvm::TargetMachine* targetMachine, OptLevel level)
{
    if (level == OptLevel::O0) {
        return;
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(targetMachine);

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::OptimizationLevel llvmLevel;
    switch (level) {
    case OptLevel::O1:
        llvmLevel = llvm::OptimizationLevel::O1;
        break;
    case OptLevel::O2:
        llvmLevel = llvm::OptimizationLevel::O2;
        break;
    case OptLevel::O3:
        llvmLevel = llvm::OptimizationLevel::O3;
        break;
    case OptLevel::Os:
        llvmLevel = llvm::OptimizationLevel::Os;
        break;
    case OptLevel::Oz:
        llvmLevel = llvm::OptimizationLevel::Oz;
        break;
    default:
        return;
    }

    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvmLevel);
    MPM.run(module, MAM);
}

} // namespace

std::string optimizeIR(const std::string& irCode, OptLevel level, std::string& errorMessage)
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parseIRModule(irCode, context, errorMessage);

    if (!module) {
        return "";
    }

    if (level != OptLevel::O0) {
        std::unique_ptr<llvm::TargetMachine> targetMachine = prepareModuleTarget(*module, "", errorMessage);
        if (!targetMachine) {
            return "";
        }
        runOptimizationPipeline(*module, targetMachine.get(), level);
    }

    std::string output;
    llvm::raw_string_ostream raw(output);
    raw << *module;
    return output;
}

bool compileToObjectMemory(
    const std::string& irCode,
    const std::string& targetTriple,
    std::vector<std::uint8_t>& output,
    OptLevel level,
    std::string& errorMessage)
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parseIRModule(irCode, context, errorMessage);

    if (!module) {
        return false;
    }

    std::unique_ptr<llvm::TargetMachine> targetMachine = prepareModuleTarget(*module, targetTriple, errorMessage);
    if (!targetMachine) {
        return false;
    }

    const std::string triple = module->getTargetTriple();

    runOptimizationPipeline(*module, targetMachine.get(), level);

    output.clear();
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream outputStream(buffer);

    llvm::legacy::PassManager passManager;
    if (targetMachine->addPassesToEmitFile(passManager, outputStream, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        errorMessage = "target '" + triple + "' can't emit an object file (no object-emission support in this build)";
        return false;
    }

    passManager.run(*module);
    if (psi::hadErrors()) return false;
    output.assign(buffer.begin(), buffer.end());
    return true;
}

} // namespace psi_codegen
