#pragma once

#include "ast.hpp"
#include <psic/compiler.hpp>
#include "logging.hpp"

#include <string>

enum class Architecture {
    X86,
    X86_64,
    AArch64,
    ARM,
    WASM32,
    WASM64,
    RISCV32,
    RISCV64,
    PPC32,
    PPC64,
    PPC64LE,
    MIPS,
    MIPSEL,
    MIPS64,
    MIPS64EL,
    LoongArch64,
    SystemZ,
};

enum class OperatingSystem {
    Linux,
    Darwin,
    Windows,
    FreeStanding,
    WASI,
};

using OptLevel = psic::OptimizationLevel;

// Codegen is split across codegen_module.cpp (declaration lowering),
// codegen_instructions.cpp (instruction/value lowering),
// codegen_registers.cpp (register tables / inline asm), and codegen.cpp
// (target machine, optimization pipeline, object emission). Their shared
// internal state lives in codegen_state.hpp.
namespace psi_codegen {

std::string compileProgram(std::string name, ProgramNode program, Architecture arch, OperatingSystem os, const std::string& targetTriple = "");

std::string optimizeIR(const std::string& irCode, OptLevel level, std::string& errorMessage);

bool compileToObjectMemory(
    const std::string& irCode,
    const std::string& targetTriple,
    std::vector<std::uint8_t>& output,
    OptLevel level,
    std::string& errorMessage);

} // namespace psi_codegen
