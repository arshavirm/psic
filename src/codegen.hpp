#pragma once

#include "ast.hpp"
#include "target_model.hpp"
#include <psic/compiler.hpp>
#include "logging.hpp"

#include <string>

using OptLevel = psic::OptimizationLevel;

// Codegen is split across codegen_module.cpp (declaration lowering),
// codegen_instructions.cpp (instruction/value lowering),
// codegen_registers.cpp (register tables / inline asm), and codegen.cpp
// (target machine, optimization pipeline, object emission). Their shared
// internal state lives in codegen_state.hpp.
namespace psi_codegen {

std::string compileProgram(std::string name, ProgramNode program, Architecture arch, OperatingSystem os, const std::string& targetTriple = "", const std::string& targetCPU = "", const std::string& targetFeatures = "");

std::string optimizeIR(const std::string& irCode, OptLevel level, std::string& errorMessage,
    const std::string& targetCPU = "", const std::string& targetFeatures = "");

// Return LLVM's normalized triple for the host executing this process.
std::string nativeTargetTriple();

// Reject JIT subtarget selectors that could emit instructions unavailable on
// the native CPU. Cross-compilation remains free to use other subtargets.
bool validateJitSubtarget(const std::string& targetTriple,
    const std::string& targetCPU,
    const std::string& targetFeatures, std::string& errorMessage);

// Return the code generator's canonical triple for an architecture/OS pair.
std::string defaultTriple(Architecture arch, OperatingSystem os);

// Get the default-address-space pointer width from the selected target ABI.
// Architecture names alone do not determine pointer width (for example, x32).
PointerModel targetPointerModel(Architecture arch, OperatingSystem os,
    const std::string& targetTriple, const std::string& targetCPU = "",
    const std::string& targetFeatures = "");
std::string resolvedTargetFeatures(const std::string& targetTriple,
    const std::string& targetCPU, const std::string& targetFeatures);

bool compileToObjectMemory(
    const std::string& irCode,
    const std::string& targetTriple,
    std::vector<std::uint8_t>& output,
    OptLevel level,
    std::string& errorMessage,
    const std::string& targetCPU = "",
    const std::string& targetFeatures = "");

bool executeJit(const std::string& irCode, const std::string& entry,
    const std::vector<psic::JitOptions::FunctionSymbol>& externalFunctions,
    std::int32_t& exitCode, std::string& errorMessage,
    const std::string& targetCPU = "", const std::string& targetFeatures = "",
    std::shared_ptr<psic::JitModule>* preparedModule = nullptr);

} // namespace psi_codegen
