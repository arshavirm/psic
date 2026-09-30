#include "target.hpp"
#include <llvm/TargetParser/Triple.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

std::string normalizeTargetTriple(const std::string& targetTriple)
{
    return llvm::Triple::normalize(targetTriple);
}

Architecture pickArchitecture(const std::string& explicitArch,
    const std::string& targetTriple)
{
    auto parse = [&](const std::string& name) {
        const std::string_view requestedName(name);
        for (const ArchitectureNameGroup& group : architectureNameGroups()) {
            if (requestedName == group.canonical
                || std::find(group.aliases.begin(), group.aliases.end(), requestedName)
                    != group.aliases.end())
                return group.architecture;
        }
        throw std::runtime_error("unknown architecture '" + name + "'");
    };
    auto parseTriple = [&]() {
        const llvm::Triple triple(llvm::Triple::normalize(targetTriple));
        switch (triple.getArch()) {
        case llvm::Triple::x86: return Architecture::X86;
        case llvm::Triple::x86_64: return Architecture::X86_64;
        case llvm::Triple::arm:
        case llvm::Triple::thumb: return Architecture::ARM;
        case llvm::Triple::aarch64: return Architecture::AArch64;
        case llvm::Triple::wasm32: return Architecture::WASM32;
        case llvm::Triple::wasm64: return Architecture::WASM64;
        case llvm::Triple::riscv32: return Architecture::RISCV32;
        case llvm::Triple::riscv64: return Architecture::RISCV64;
        case llvm::Triple::ppc: return Architecture::PPC32;
        case llvm::Triple::ppc64: return Architecture::PPC64;
        case llvm::Triple::ppc64le: return Architecture::PPC64LE;
        case llvm::Triple::mips: return Architecture::MIPS;
        case llvm::Triple::mipsel: return Architecture::MIPSEL;
        case llvm::Triple::mips64: return Architecture::MIPS64;
        case llvm::Triple::mips64el: return Architecture::MIPS64EL;
        case llvm::Triple::loongarch64: return Architecture::LoongArch64;
        case llvm::Triple::systemz: return Architecture::SystemZ;
        default:
            if (triple.getArch() != llvm::Triple::UnknownArch)
                return Architecture::LLVMGeneric;
            throw std::runtime_error("unsupported architecture in target triple '" + targetTriple + "'");
        }
    };
    if (!explicitArch.empty()) {
        Architecture arch = parse(explicitArch);
        if (!targetTriple.empty() && arch != parseTriple())
            throw std::runtime_error("architecture conflicts with target triple");
        return arch;
    }
    return targetTriple.empty() ? Architecture::X86_64 : parseTriple();
}

OperatingSystem pickOperatingSystem(const std::string& explicitOs,
    const std::string& targetTriple)
{
    if (!explicitOs.empty()) {
        const std::string_view requestedName(explicitOs);
        for (const OperatingSystemNameGroup& group : operatingSystemNameGroups()) {
            if (requestedName == group.canonical
                || std::find(group.aliases.begin(), group.aliases.end(), requestedName)
                    != group.aliases.end())
                return group.operatingSystem;
        }
        throw std::runtime_error("unknown OS '" + explicitOs + "'");
    }

    if (!targetTriple.empty()) {
        const llvm::Triple triple(llvm::Triple::normalize(targetTriple));
        if (triple.isOSLinux()) return OperatingSystem::Linux;
        if (triple.isOSDarwin()) return OperatingSystem::Darwin;
        if (triple.isOSWindows()) return OperatingSystem::Windows;
        if (triple.getOS() == llvm::Triple::WASI) return OperatingSystem::WASI;
        if (triple.getOS() == llvm::Triple::UnknownOS
            && (triple.getOSName().empty() || triple.getOSName() == "unknown"
                || triple.getOSName() == "none"))
            return OperatingSystem::FreeStanding;
        if (triple.getOS() != llvm::Triple::UnknownOS)
            return OperatingSystem::TargetSpecific;
        throw std::runtime_error("unsupported OS in target triple '" + targetTriple + "'");
    }

    return OperatingSystem::Linux;
}
