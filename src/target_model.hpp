#pragma once

#include <string>
#include <string_view>

enum class Architecture {
    X86, X86_64, AArch64, ARM, WASM32, WASM64, RISCV32, RISCV64,
    PPC32, PPC64, PPC64LE, MIPS, MIPSEL, MIPS64, MIPS64EL,
    LoongArch64, SystemZ, LLVMGeneric,
};

enum class OperatingSystem {
    Linux, Darwin, Windows, FreeStanding, WASI, TargetSpecific,
};

struct PointerModel {
    unsigned bits = 64;
    bool integral = true;
    Architecture architecture = Architecture::X86_64;
    OperatingSystem operatingSystem = OperatingSystem::Linux;
    std::string effectiveTargetFeatures;
};

inline bool targetFeatureEnabled(std::string_view features, std::string_view name)
{
    bool enabled = false;
    std::size_t begin = 0;
    while (begin < features.size()) {
        std::size_t end = features.find(',', begin);
        if (end == std::string_view::npos) end = features.size();
        std::string_view feature = features.substr(begin, end - begin);
        while (!feature.empty() && feature.front() == ' ') feature.remove_prefix(1);
        while (!feature.empty() && feature.back() == ' ') feature.remove_suffix(1);
        if (feature.size() > 1 && feature.substr(1) == name) {
            if (feature.front() == '+') enabled = true;
            else if (feature.front() == '-') enabled = false;
        }
        begin = end + 1;
    }
    return enabled;
}

inline bool targetFeatureDisabled(std::string_view features, std::string_view name)
{
    bool disabled = false;
    std::size_t begin = 0;
    while (begin < features.size()) {
        std::size_t end = features.find(',', begin);
        if (end == std::string_view::npos) end = features.size();
        std::string_view feature = features.substr(begin, end - begin);
        while (!feature.empty() && feature.front() == ' ') feature.remove_prefix(1);
        while (!feature.empty() && feature.back() == ' ') feature.remove_suffix(1);
        if (feature.size() > 1 && feature.substr(1) == name) {
            if (feature.front() == '-') disabled = true;
            else if (feature.front() == '+') disabled = false;
        }
        begin = end + 1;
    }
    return disabled;
}

inline bool supportsNativeSpecialInstruction(Architecture architecture,
    std::string_view name)
{
    switch (architecture) {
    case Architecture::X86:
    case Architecture::X86_64:
        return name == "pause" || name == "lfence" || name == "sfence"
            || name == "mfence" || name == "hlt" || name == "cli"
            || name == "sti" || name == "inb" || name == "outb"
            || name == "rdtsc";
    case Architecture::ARM:
        return name == "yield" || name == "dmb" || name == "dsb"
            || name == "isb" || name == "wfi" || name == "wfe" || name == "sev";
    case Architecture::AArch64:
        return name == "yield" || name == "dmb" || name == "dsb"
            || name == "isb" || name == "wfi" || name == "wfe"
            || name == "sev" || name == "sevl";
    case Architecture::RISCV32:
    case Architecture::RISCV64:
        return name == "ecall" || name == "ebreak" || name == "wfi"
            || name == "fence_i";
    case Architecture::PPC32:
    case Architecture::PPC64:
    case Architecture::PPC64LE:
        return name == "sync" || name == "lwsync" || name == "isync"
            || name == "eieio";
    case Architecture::MIPS:
    case Architecture::MIPSEL:
    case Architecture::MIPS64:
    case Architecture::MIPS64EL:
        return name == "sync";
    case Architecture::LoongArch64:
        return name == "dbar" || name == "ibar";
    case Architecture::SystemZ:
        return name == "serialize";
    case Architecture::WASM32:
    case Architecture::WASM64:
    case Architecture::LLVMGeneric:
        return false;
    }
    return false;
}

inline bool supportsLinuxSyscall(Architecture architecture)
{
    return architecture == Architecture::X86 || architecture == Architecture::X86_64
        || architecture == Architecture::ARM || architecture == Architecture::AArch64
        || architecture == Architecture::RISCV32 || architecture == Architecture::RISCV64;
}

inline unsigned linuxSyscallWordBits(Architecture architecture)
{
    switch (architecture) {
    case Architecture::X86:
    case Architecture::ARM:
    case Architecture::RISCV32:
        return 32;
    case Architecture::X86_64:
    case Architecture::AArch64:
    case Architecture::RISCV64:
        return 64;
    default:
        return 0;
    }
}
