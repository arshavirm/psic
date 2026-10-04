#include "codegen_state.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/MC/MCSubtargetInfo.h>
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
#include <unordered_set>

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

std::unique_ptr<llvm::TargetMachine> buildTargetMachine(const std::string& triple,
    std::string& errorMessage, const std::string& cpu, const std::string& requestedFeatures)
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
    if (!requestedFeatures.empty()) {
        if (!features.empty()) features += ",";
        features += requestedFeatures;
    }
    const std::string selectedCPU = cpu.empty() ? "generic" : cpu;
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(triple, selectedCPU, features, options, relocModel));

    if (!targetMachine) {
        errorMessage = "failed to create a target machine for '" + triple + "'";
    }
    return targetMachine;
}

PointerModel targetPointerModel(Architecture arch, OperatingSystem os,
    const std::string& targetTriple, const std::string& targetCPU,
    const std::string& targetFeatures)
{
    const std::string triple = targetTriple.empty()
        ? defaultTriple(arch, os) : targetTriple;
    std::string error;
    auto targetMachine = buildTargetMachine(triple, error, targetCPU, targetFeatures);
    if (!targetMachine)
        throw std::runtime_error(error.empty()
            ? "failed to determine pointer width for target '" + triple + "'" : error);
    const llvm::DataLayout layout = targetMachine->createDataLayout();
    return {layout.getPointerSizeInBits(0), !layout.isNonIntegralAddressSpace(0), arch, os,
        resolvedTargetFeatures(triple, targetCPU,
            targetMachine->getTargetFeatureString().str())};
}

std::string resolvedTargetFeatures(const std::string& targetTriple,
    const std::string& targetCPU, const std::string& targetFeatures)
{
    const llvm::Triple parsedTriple(targetTriple);
    if (parsedTriple.getArch() != llvm::Triple::riscv32
        && parsedTriple.getArch() != llvm::Triple::riscv64)
        return targetFeatures;
    initializeAllTargetComponents();
    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, lookupError);
    if (!target) return targetFeatures;
    auto subtarget = std::unique_ptr<llvm::MCSubtargetInfo>(target->createMCSubtargetInfo(
        targetTriple, targetCPU.empty() ? "generic" : targetCPU, targetFeatures));
    if (!subtarget) return targetFeatures;

    std::string resolved;
    for (const char* feature : {"f", "d", "zicsr", "zicntr", "zifencei"}) {
        if (!resolved.empty()) resolved += ',';
        const std::string query = "+" + std::string(feature);
        resolved += subtarget->checkFeatures(query) ? '+' : '-';
        resolved += feature;
    }
    return resolved;
}

std::string nativeTargetTriple()
{
    return llvm::sys::getDefaultTargetTriple();
}

bool validateJitSubtarget(const std::string& targetTriple,
    const std::string& targetCPU,
    const std::string& targetFeatures, std::string& errorMessage)
{
    const std::string nativeTriple = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple requested(llvm::Triple::normalize(targetTriple));
    const llvm::Triple native(nativeTriple);
    if (requested.getArch() != native.getArch() || requested.getOS() != native.getOS()
        || requested.getEnvironment() != native.getEnvironment()
        || requested.getObjectFormat() != native.getObjectFormat()) {
        errorMessage = "JIT execution requires the native target triple '" + nativeTriple + "'";
        return false;
    }
    if (!targetCPU.empty() && targetCPU != "generic") {
        const llvm::StringRef hostCPU = llvm::sys::getHostCPUName();
        if (hostCPU.empty() || targetCPU != hostCPU) {
            errorMessage = "JIT CPU '" + targetCPU
                + "' does not match the detected host CPU '" + hostCPU.str() + "'";
            return false;
        }
    }
    if (targetFeatures.empty()) return true;

    llvm::StringMap<bool, llvm::MallocAllocator> hostFeatures;
    if (!llvm::sys::getHostCPUFeatures(hostFeatures)) {
        errorMessage = "JIT cannot verify requested CPU features against the host";
        return false;
    }
    llvm::StringRef features(targetFeatures);
    while (!features.empty()) {
        const std::size_t separator = features.find(',');
        const llvm::StringRef item = features.take_front(separator).trim();
        if (item.size() < 2 || (item.front() != '+' && item.front() != '-')) {
            errorMessage = "invalid JIT CPU feature override '" + item.str() + "'";
            return false;
        }
        if (item.front() == '+') {
            const auto hostFeature = hostFeatures.find(item.drop_front());
            if (hostFeature == hostFeatures.end() || !hostFeature->second) {
                errorMessage = "JIT CPU feature '" + item.drop_front().str()
                    + "' is not supported by the host";
                return false;
            }
        }
        if (separator == llvm::StringRef::npos) break;
        features = features.drop_front(separator + 1);
    }
    return true;
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
    llvm::Module& module, std::string triple, std::string& errorMessage,
    const std::string& cpu = "", const std::string& features = "")
{
    if (triple.empty()) triple = module.getTargetTriple();
    if (triple.empty()) triple = llvm::sys::getDefaultTargetTriple();

    std::unique_ptr<llvm::TargetMachine> targetMachine = buildTargetMachine(triple, errorMessage, cpu, features);
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

std::string optimizeIR(const std::string& irCode, OptLevel level, std::string& errorMessage,
    const std::string& targetCPU, const std::string& targetFeatures)
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parseIRModule(irCode, context, errorMessage);

    if (!module) {
        return "";
    }

    if (level != OptLevel::O0) {
        std::unique_ptr<llvm::TargetMachine> targetMachine = prepareModuleTarget(*module, "", errorMessage, targetCPU, targetFeatures);
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
    std::string& errorMessage,
    const std::string& targetCPU,
    const std::string& targetFeatures)
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parseIRModule(irCode, context, errorMessage);

    if (!module) {
        return false;
    }

    std::unique_ptr<llvm::TargetMachine> targetMachine = prepareModuleTarget(*module, targetTriple, errorMessage, targetCPU, targetFeatures);
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

class NativeJitModule final : public psic::JitModule {
public:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::ExecutionEngine> engine;
    std::unordered_map<std::string, std::uintptr_t> functions;

    std::uintptr_t functionAddress(const std::string& name) const noexcept override
    {
        auto found = functions.find(name);
        return found == functions.end() ? 0 : found->second;
    }
};

bool executeJit(const std::string& irCode, const std::string& entry,
    const std::vector<psic::JitOptions::FunctionSymbol>& externalFunctions,
    std::int32_t& exitCode, std::string& errorMessage,
    const std::string& targetCPU, const std::string& targetFeatures,
    std::shared_ptr<psic::JitModule>* preparedModule)
{
    static std::mutex jitMutex;
    std::unique_lock<std::mutex> lock(jitMutex);
    initializeAllTargetComponents();
    auto ownedContext = std::make_unique<llvm::LLVMContext>();
    auto module = parseIRModule(irCode, *ownedContext, errorMessage);
    if (!module) return false;
    const std::string triple = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple requested(module->getTargetTriple());
    const llvm::Triple native(triple);
    if (requested.getArch() != native.getArch() || requested.getOS() != native.getOS()
        || requested.getEnvironment() != native.getEnvironment()
        || requested.getObjectFormat() != native.getObjectFormat()) {
        errorMessage = "JIT execution requires the native target triple '" + triple + "'";
        return false;
    }
    if (module->getDataLayout().isDefault()) {
        errorMessage = "JIT module is missing its target data layout";
        return false;
    }
    if (!validateJitSubtarget(triple, targetCPU, targetFeatures, errorMessage)) return false;
    auto machine = buildTargetMachine(triple, errorMessage, targetCPU, targetFeatures);
    if (!machine) return false;
    const std::string moduleLayout = module->getDataLayout().getStringRepresentation();
    const std::string nativeLayout = machine->createDataLayout().getStringRepresentation();
    if (moduleLayout != nativeLayout) {
        errorMessage = "JIT target data layout does not match the native execution ABI";
        return false;
    }
    module->setTargetTriple(triple);
    auto* function = module->getFunction(entry);
    if (!preparedModule && (!function || function->empty())) {
        errorMessage = "JIT entry '" + entry + "' is not a defined function";
        return false;
    }
    if (!preparedModule && (function->arg_size() != 0 || (!function->getReturnType()->isVoidTy()
        && !function->getReturnType()->isIntegerTy(32)))) {
        errorMessage = "JIT entry must take no arguments and return void or i32";
        return false;
    }
    std::unordered_set<std::string> mappedNames;
    std::vector<llvm::Function*> mappedDeclarations;
    mappedDeclarations.reserve(externalFunctions.size());
    for (const auto& symbol : externalFunctions) {
        if (symbol.name.empty() || !symbol.address) {
            errorMessage = "JIT external function mappings require a name and non-null address";
            return false;
        }
        if (!mappedNames.insert(symbol.name).second) {
            errorMessage = "duplicate JIT external function mapping for '" + symbol.name + "'";
            return false;
        }
        llvm::Function* declaration = module->getFunction(symbol.name);
        if (!declaration || !declaration->isDeclaration() || declaration->isIntrinsic()) {
            errorMessage = "JIT external function mapping '" + symbol.name
                + "' does not name a PSI external function declaration";
            return false;
        }
        mappedDeclarations.push_back(declaration);
    }
    for (const llvm::Function& candidate : *module) {
        if (candidate.isDeclaration() && !candidate.isIntrinsic()
            && mappedNames.find(candidate.getName().str()) == mappedNames.end()) {
            errorMessage = "JIT external function '" + candidate.getName().str()
                + "' has no explicit host mapping";
            return false;
        }
    }
    std::vector<std::string> definedFunctions;
    for (const llvm::Function& candidate : *module)
        if (!candidate.isDeclaration()) definedFunctions.push_back(candidate.getName().str());
    llvm::EngineBuilder builder(std::move(module));
    builder.setEngineKind(llvm::EngineKind::JIT);
    builder.setMCPU(targetCPU.empty() ? "generic" : targetCPU);
    std::vector<std::string> jitFeatures;
    std::string featureString = machine->getTargetFeatureString().str();
    for (std::size_t begin = 0; begin < featureString.size();) {
        const std::size_t end = featureString.find(',', begin);
        jitFeatures.push_back(featureString.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    builder.setMAttrs(jitFeatures);
    machine.reset();
    builder.setErrorStr(&errorMessage);
    builder.setMCJITMemoryManager(std::make_unique<llvm::SectionMemoryManager>());
    std::unique_ptr<llvm::ExecutionEngine> engine(builder.create());
    if (!engine) return false;
    for (std::size_t index = 0; index < externalFunctions.size(); ++index) {
        engine->addGlobalMapping(mappedDeclarations[index], externalFunctions[index].address);
    }
    auto prepared = std::make_shared<NativeJitModule>();
    prepared->context = std::move(ownedContext);
    prepared->engine = std::move(engine);
    for (const auto& name : definedFunctions) {
        const auto address = prepared->engine->getFunctionAddress(name);
        if (!address || prepared->engine->hasError()) {
            errorMessage = "JIT could not resolve function '" + name + "': "
                + prepared->engine->getErrorMessage();
            return false;
        }
        prepared->functions.emplace(name, static_cast<std::uintptr_t>(address));
    }
    if (preparedModule) {
        *preparedModule = std::move(prepared);
        return true;
    }
    const auto address = prepared->functionAddress(entry);
    const bool returnsVoid = function->getReturnType()->isVoidTy();
    lock.unlock();
    if (returnsVoid) {
        reinterpret_cast<void (*)()>(address)();
        exitCode = 0;
    } else {
        exitCode = reinterpret_cast<std::int32_t (*)()>(address)();
    }
    return true;
}

} // namespace psi_codegen
