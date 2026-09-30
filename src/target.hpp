#pragma once
#include "codegen.hpp"

#include <string_view>
#include <vector>

enum class ArchitectureProfileFamily {
    GeneralPurpose,
    LinuxOrFreestanding,
    WebAssembly,
    GenericLLVM,
};

struct ArchitectureNameGroup {
    Architecture architecture;
    std::string_view canonical;
    std::vector<std::string_view> aliases;
    std::string_view description;
    ArchitectureProfileFamily profileFamily;
};

struct OperatingSystemNameGroup {
    OperatingSystem operatingSystem;
    std::string_view canonical;
    std::vector<std::string_view> aliases;
    std::string_view description;
};

inline const std::vector<ArchitectureNameGroup>& architectureNameGroups()
{
    static const std::vector<ArchitectureNameGroup> groups = {
        {Architecture::X86, "x86", {"i386", "i486", "i586", "i686"},
            "32-bit x86", ArchitectureProfileFamily::GeneralPurpose},
        {Architecture::X86_64, "x86_64", {"x86-64", "amd64"},
            "64-bit x86", ArchitectureProfileFamily::GeneralPurpose},
        {Architecture::AArch64, "aarch64", {"arm64"},
            "64-bit ARM", ArchitectureProfileFamily::GeneralPurpose},
        {Architecture::ARM, "arm", {"armv7", "armv7a", "thumbv7", "thumbv7a"},
            "32-bit ARM", ArchitectureProfileFamily::GeneralPurpose},
        {Architecture::WASM32, "wasm32", {"wasm"},
            "32-bit WebAssembly (default OS: none)", ArchitectureProfileFamily::WebAssembly},
        {Architecture::WASM64, "wasm64", {},
            "64-bit WebAssembly (default OS: none)", ArchitectureProfileFamily::WebAssembly},
        {Architecture::RISCV32, "riscv32", {},
            "32-bit RISC-V", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::RISCV64, "riscv64", {},
            "64-bit RISC-V", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::PPC32, "ppc", {"powerpc"},
            "32-bit PowerPC", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::PPC64, "ppc64", {"powerpc64"},
            "64-bit big-endian PowerPC", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::PPC64LE, "ppc64le", {"powerpc64le"},
            "64-bit little-endian PowerPC", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::MIPS, "mips", {},
            "32-bit big-endian MIPS", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::MIPSEL, "mipsel", {},
            "32-bit little-endian MIPS", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::MIPS64, "mips64", {},
            "64-bit big-endian MIPS", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::MIPS64EL, "mips64el", {},
            "64-bit little-endian MIPS", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::LoongArch64, "loongarch64", {},
            "64-bit LoongArch", ArchitectureProfileFamily::LinuxOrFreestanding},
        {Architecture::SystemZ, "s390x", {"systemz"},
            "64-bit SystemZ", ArchitectureProfileFamily::LinuxOrFreestanding},
    };
    return groups;
}

inline ArchitectureProfileFamily architectureProfileFamily(Architecture architecture)
{
    for (const auto& group : architectureNameGroups())
        if (group.architecture == architecture) return group.profileFamily;
    return ArchitectureProfileFamily::GenericLLVM;
}

inline const std::vector<OperatingSystemNameGroup>& operatingSystemNameGroups()
{
    static const std::vector<OperatingSystemNameGroup> groups = {
        {OperatingSystem::Linux, "linux", {}, "Linux (default)"},
        {OperatingSystem::Darwin, "darwin", {"macos", "macosx"}, "macOS"},
        {OperatingSystem::Windows, "windows", {"win32"}, "Windows"},
        {OperatingSystem::WASI, "wasi", {}, "WebAssembly System Interface"},
        {OperatingSystem::FreeStanding, "none", {"freestanding", "bare-metal"}, "Bare metal / no OS"},
    };
    return groups;
}

Architecture pickArchitecture(const std::string&, const std::string&);
OperatingSystem pickOperatingSystem(const std::string&, const std::string&);
std::string normalizeTargetTriple(const std::string&);
