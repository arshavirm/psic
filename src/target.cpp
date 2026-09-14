#include "target.hpp"
#include <llvm/TargetParser/Triple.h>
#include <stdexcept>
#include <utility>

Architecture pickArchitecture(const std::string& explicitArch,
    const std::string& targetTriple)
{
    static const std::pair<const char*, Architecture> names[] = {
        {"x86", Architecture::X86}, {"i386", Architecture::X86},
        {"i486", Architecture::X86}, {"i586", Architecture::X86}, {"i686", Architecture::X86},
        {"x86_64", Architecture::X86_64}, {"x86-64", Architecture::X86_64}, {"amd64", Architecture::X86_64},
        {"arm", Architecture::ARM}, {"armv7", Architecture::ARM}, {"armv7a", Architecture::ARM},
        {"thumbv7", Architecture::ARM}, {"thumbv7a", Architecture::ARM},
        {"aarch64", Architecture::AArch64}, {"arm64", Architecture::AArch64},
        {"wasm", Architecture::WASM32}, {"wasm32", Architecture::WASM32}, {"wasm64", Architecture::WASM64},
        {"riscv32", Architecture::RISCV32}, {"riscv64", Architecture::RISCV64},
        {"ppc", Architecture::PPC32}, {"powerpc", Architecture::PPC32},
        {"ppc64", Architecture::PPC64}, {"powerpc64", Architecture::PPC64},
        {"ppc64le", Architecture::PPC64LE}, {"powerpc64le", Architecture::PPC64LE},
        {"mips", Architecture::MIPS}, {"mipsel", Architecture::MIPSEL},
        {"mips64", Architecture::MIPS64}, {"mips64el", Architecture::MIPS64EL},
        {"loongarch64", Architecture::LoongArch64}, {"s390x", Architecture::SystemZ},
        {"systemz", Architecture::SystemZ}
    };
    auto parse = [&](const std::string& name) {
        for (const auto& item : names)
            if (name == item.first) return item.second;
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
        default: throw std::runtime_error("unsupported architecture in target triple '" + targetTriple + "'");
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
    if (explicitOs == "wasi") return OperatingSystem::WASI;

    if (explicitOs == "linux") {
        return OperatingSystem::Linux;
    }

    if (explicitOs == "darwin" || explicitOs == "macos" || explicitOs == "macosx") {
        return OperatingSystem::Darwin;
    }

    if (explicitOs == "windows" || explicitOs == "win32") {
        return OperatingSystem::Windows;
    }

    if (explicitOs == "none" || explicitOs == "freestanding" || explicitOs == "bare-metal") {
        return OperatingSystem::FreeStanding;
    }

    if (!explicitOs.empty()) {
        throw std::runtime_error("unknown OS '" + explicitOs + "'");
    }

    if (!targetTriple.empty()) {
        const llvm::Triple triple(llvm::Triple::normalize(targetTriple));
        if (triple.isOSLinux()) return OperatingSystem::Linux;
        if (triple.isOSDarwin()) return OperatingSystem::Darwin;
        if (triple.isOSWindows()) return OperatingSystem::Windows;
        if (triple.getOS() == llvm::Triple::WASI) return OperatingSystem::WASI;
        if (triple.getOSName().empty() || triple.getOSName() == "unknown" || triple.getOSName() == "none")
            return OperatingSystem::FreeStanding;
        throw std::runtime_error("unsupported OS in target triple '" + targetTriple + "'");
    }

    return OperatingSystem::Linux;
}

