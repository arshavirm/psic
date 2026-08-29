#pragma once

#include "ast.hpp"
#include "logging.hpp"

#include <string>

enum class Architecture {
    X86,
    X86_64,
    AArch64,
    ARM
};

enum class OperatingSystem {
    Linux,
    Darwin,
    Windows,
    FreeStanding,
};

enum class OptLevel {
    O0,
    O1,
    O2,
    O3,
    Os,
    Oz,
};

std::string compileProgram(std::string name, ProgramNode program, Architecture arch, OperatingSystem os);

std::string optimizeIR(const std::string& irCode, OptLevel level, std::string& errorMessage);

bool compileToObjectFile(
    const std::string& irCode,
    const std::string& targetTriple,
    const std::string& outputPath,
    OptLevel level,
    std::string& errorMessage);