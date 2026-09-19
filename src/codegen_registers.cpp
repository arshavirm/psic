#include "codegen_state.hpp"

#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/IntrinsicsWebAssembly.h>
#include <llvm/IR/IntrinsicsX86.h>

namespace psi_codegen {

llvm::Type* widthToType(RegisterWidth width, llvm::LLVMContext& context)
{
    switch (width) {
    case RegisterWidth::I8:
        return llvm::Type::getInt8Ty(context);
    case RegisterWidth::I16:
        return llvm::Type::getInt16Ty(context);
    case RegisterWidth::I32:
        return llvm::Type::getInt32Ty(context);
    case RegisterWidth::I64:
        return llvm::Type::getInt64Ty(context);
    case RegisterWidth::F32:
        return llvm::Type::getFloatTy(context);
    case RegisterWidth::F64:
        return llvm::Type::getDoubleTy(context);
    }
    return nullptr;
}

namespace {

void addRegister(State& s, const std::string& name, RegisterWidth width)
{
    s.specialRegisters[name] = SpecialRegisterInfo { "{" + name + "}", width, "", "" };
}

} // namespace

void buildSpecialRegisterTable(State& s)
{
    const Architecture arch = s.architecture;
    s.specialRegisters.clear();

    if (arch == Architecture::X86_64) {
        static const char* gpr64[] = {
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
        };
        static const char* gpr32[] = {
            "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
        };
        static const char* gpr16[] = {
            "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
            "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
        };
        static const char* gpr8[] = {
            "al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
            "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
        };

        for (auto name : gpr64)
            addRegister(s, name, RegisterWidth::I64);
        for (auto name : gpr32)
            addRegister(s, name, RegisterWidth::I32);
        for (auto name : gpr16)
            addRegister(s, name, RegisterWidth::I16);
        for (auto name : gpr8)
            addRegister(s, name, RegisterWidth::I8);

        for (int i = 0; i < 16; ++i)
            addRegister(s, "xmm" + std::to_string(i), RegisterWidth::F32);
    } else if (arch == Architecture::X86) {
        static const char* gpr32[] = {
            "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"
        };
        static const char* gpr16[] = {
            "ax", "bx", "cx", "dx", "si", "di", "bp", "sp"
        };
        static const char* gpr8[] = {
            "al", "bl", "cl", "dl"
        };

        for (auto name : gpr32)
            addRegister(s, name, RegisterWidth::I32);
        for (auto name : gpr16)
            addRegister(s, name, RegisterWidth::I16);
        for (auto name : gpr8)
            addRegister(s, name, RegisterWidth::I8);

        for (int i = 0; i < 8; ++i)
            addRegister(s, "xmm" + std::to_string(i), RegisterWidth::F32);
    } else if (isAArch64(arch)) {
        for (int i = 0; i < 31; ++i) {
            addRegister(s, "x" + std::to_string(i), RegisterWidth::I64);
            addRegister(s, "w" + std::to_string(i), RegisterWidth::I32);
        }

        for (int i = 0; i < 32; ++i) {
            addRegister(s, "s" + std::to_string(i), RegisterWidth::F32);
            addRegister(s, "d" + std::to_string(i), RegisterWidth::F64);
        }

        addRegister(s, "sp", RegisterWidth::I64);

        addRegister(s, "fp", RegisterWidth::I64);
        addRegister(s, "lr", RegisterWidth::I64);
    } else if (isARM32(arch)) {
        for (int i = 0; i < 13; ++i)
            addRegister(s, "r" + std::to_string(i), RegisterWidth::I32);

        addRegister(s, "sp", RegisterWidth::I32);
        addRegister(s, "lr", RegisterWidth::I32);

        addRegister(s, "fp", RegisterWidth::I32);

        for (int i = 0; i < 32; ++i)
            addRegister(s, "s" + std::to_string(i), RegisterWidth::F32);

        for (int i = 0; i < 16; ++i)
            addRegister(s, "d" + std::to_string(i), RegisterWidth::F64);
    }
    if (arch == Architecture::RISCV32 || arch == Architecture::RISCV64) {
        auto width = arch == Architecture::RISCV64 ? RegisterWidth::I64 : RegisterWidth::I32;
        static const char* aliases[] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
            "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
            "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
        for (int i = 0; i < 32; ++i) {
            std::string name = "x" + std::to_string(i);
            addRegister(s, name, width);
            s.specialRegisters[aliases[i]] = s.specialRegisters[name];
        }
        s.specialRegisters["fp"] = s.specialRegisters["x8"];
    }
    if (arch == Architecture::PPC32 || arch == Architecture::PPC64 || arch == Architecture::PPC64LE) {
        for (int i = 0; i < 32; ++i) {
            addRegister(s, "r" + std::to_string(i), arch == Architecture::PPC32 ? RegisterWidth::I32 : RegisterWidth::I64);
            addRegister(s, "f" + std::to_string(i), RegisterWidth::F64);
        }
        s.specialRegisters["sp"] = s.specialRegisters["r1"];
    }
    if (arch == Architecture::MIPS || arch == Architecture::MIPSEL
        || arch == Architecture::MIPS64 || arch == Architecture::MIPS64EL) {
        const bool wide = arch == Architecture::MIPS64 || arch == Architecture::MIPS64EL;
        static const char* aliases[] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
            "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3",
            "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        for (int i = 0; i < 32; ++i) {
            std::string name = "r" + std::to_string(i);
            s.specialRegisters[name] = {"{$" + std::to_string(i) + "}", wide ? RegisterWidth::I64 : RegisterWidth::I32, "", ""};
            if (!wide || i < 8 || i >= 16) s.specialRegisters[aliases[i]] = s.specialRegisters[name];
        }
        if (wide) {
            for (int i = 4; i < 8; ++i) s.specialRegisters["a" + std::to_string(i)] = s.specialRegisters["r" + std::to_string(i + 4)];
            for (int i = 0; i < 4; ++i) s.specialRegisters["t" + std::to_string(i)] = s.specialRegisters["r" + std::to_string(i + 12)];
        }
    }
    if (arch == Architecture::LoongArch64) {
        for (int i = 0; i < 32; ++i) addRegister(s, "r" + std::to_string(i), RegisterWidth::I64);
        s.specialRegisters["zero"] = s.specialRegisters["r0"];
        s.specialRegisters["ra"] = s.specialRegisters["r1"];
        s.specialRegisters["tp"] = s.specialRegisters["r2"];
        s.specialRegisters["sp"] = s.specialRegisters["r3"];
        s.specialRegisters["fp"] = s.specialRegisters["r22"];
        for (int i = 0; i < 8; ++i) s.specialRegisters["a" + std::to_string(i)] = s.specialRegisters["r" + std::to_string(i + 4)];
    }
    if (arch == Architecture::SystemZ) {
        for (int i = 0; i < 16; ++i) {
            addRegister(s, "r" + std::to_string(i), RegisterWidth::I64);
            addRegister(s, "f" + std::to_string(i), RegisterWidth::F64);
        }
        s.specialRegisters["sp"] = s.specialRegisters["r15"];
    }

    auto systemRegister = [&](const std::string& name, RegisterWidth width,
                              const std::string& read, const std::string& write = "") {
        s.specialRegisters[name] = {"r", width, read, write};
    };
    if (isAArch64(arch)) {
        systemRegister("nzcv", RegisterWidth::I64, "mrs $0, NZCV", "msr NZCV, $0");
        systemRegister("fpcr", RegisterWidth::I64, "mrs $0, FPCR", "msr FPCR, $0");
        systemRegister("fpsr", RegisterWidth::I64, "mrs $0, FPSR", "msr FPSR, $0");
        systemRegister("cntvct_el0", RegisterWidth::I64, "mrs $0, CNTVCT_EL0");
        systemRegister("cntfrq_el0", RegisterWidth::I64, "mrs $0, CNTFRQ_EL0");
        systemRegister("tpidr_el0", RegisterWidth::I64, "mrs $0, TPIDR_EL0", "msr TPIDR_EL0, $0");
    }
    if (isARM32(arch)) {
        s.specialRegisters["r13"] = s.specialRegisters["sp"];
        s.specialRegisters["r14"] = s.specialRegisters["lr"];
        systemRegister("apsr", RegisterWidth::I32, "mrs $0, APSR", "msr APSR_nzcvq, $0");
    }
    if (arch == Architecture::RISCV32 || arch == Architecture::RISCV64) {
        auto width = arch == Architecture::RISCV64 ? RegisterWidth::I64 : RegisterWidth::I32;
        systemRegister("cycle", width, "rdcycle $0");
        systemRegister("time", width, "rdtime $0");
        systemRegister("instret", width, "rdinstret $0");
        if (arch == Architecture::RISCV32) {
            systemRegister("cycleh", width, "rdcycleh $0");
            systemRegister("timeh", width, "rdtimeh $0");
            systemRegister("instreth", width, "rdinstreth $0");
        }
    }
    if (arch == Architecture::PPC32 || arch == Architecture::PPC64 || arch == Architecture::PPC64LE) {
        auto width = arch == Architecture::PPC32 ? RegisterWidth::I32 : RegisterWidth::I64;
        systemRegister("lr", width, "mflr $0", "mtlr $0");
        systemRegister("ctr", width, "mfctr $0", "mtctr $0");
        systemRegister("xer", width, "mfxer $0", "mtxer $0");
    }
    if (arch == Architecture::MIPS || arch == Architecture::MIPSEL
        || arch == Architecture::MIPS64 || arch == Architecture::MIPS64EL) {
        auto width = arch == Architecture::MIPS64 || arch == Architecture::MIPS64EL ? RegisterWidth::I64 : RegisterWidth::I32;
        systemRegister("hi", width, "mfhi $0", "mthi $0");
        systemRegister("lo", width, "mflo $0", "mtlo $0");
    }

}

llvm::Value* wasmMemorySize(State& s, llvm::IRBuilder<>* builder)
{
    auto* word = builder->getIntNTy(s.architecture == Architecture::WASM64 ? 64 : 32);
    auto* fn = llvm::Intrinsic::getDeclaration(builder->GetInsertBlock()->getModule(),
        llvm::Intrinsic::wasm_memory_size, {word});
    return builder->CreateCall(fn, {builder->getInt32(0)});
}

llvm::Value* processSpecialRegisterRead(State& s, const SpecialRegNode& reg, llvm::IRBuilder<>* builder)
{
    if (reg.name == "mxcsr" && (s.architecture == Architecture::X86 || s.architecture == Architecture::X86_64)) {
        auto* slot = builder->CreateAlloca(builder->getInt32Ty());
        auto* fn = llvm::Intrinsic::getDeclaration(builder->GetInsertBlock()->getModule(), llvm::Intrinsic::x86_sse_stmxcsr);
        builder->CreateCall(fn, {slot});
        return builder->CreateLoad(builder->getInt32Ty(), slot);
    }
    if (isWasm(s) && reg.name == "memory_pages") return wasmMemorySize(s, builder);
    auto it = s.specialRegisters.find(reg.name);
    if (it == s.specialRegisters.end()) {
        psi::ErrorStream() << "'%" << reg.name << "' isn't a recognized register for this target\n";
        return nullptr;
    }

    llvm::Type* type = widthToType(it->second.width, *s.context);
    auto* asmType = llvm::FunctionType::get(type, false);
    auto* asmFn = llvm::InlineAsm::get(asmType, it->second.readAsm, "=" + it->second.constraint, true);
    return builder->CreateCall(asmFn);
}

void processSpecialRegisterWrite(State& s, const SpecialRegNode& reg, llvm::Value* value, llvm::IRBuilder<>* builder)
{
    if (reg.name == "mxcsr" && (s.architecture == Architecture::X86 || s.architecture == Architecture::X86_64)) {
        if (!value->getType()->isIntegerTy()) {
            psi::ErrorStream() << "'%mxcsr' requires an integer\n";
            return;
        }
        auto* slot = builder->CreateAlloca(builder->getInt32Ty());
        builder->CreateStore(builder->CreateZExtOrTrunc(value, builder->getInt32Ty()), slot);
        auto* fn = llvm::Intrinsic::getDeclaration(builder->GetInsertBlock()->getModule(), llvm::Intrinsic::x86_sse_ldmxcsr);
        builder->CreateCall(fn, {slot});
        return;
    }
    if (isWasm(s) && reg.name == "memory_pages") {
        psi::ErrorStream() << "'%memory_pages' is read-only; use #memory_grow\n";
        return;
    }
    auto it = s.specialRegisters.find(reg.name);
    if (it == s.specialRegisters.end()) {
        psi::ErrorStream() << "'%" << reg.name << "' isn't a recognized register for this target\n";
        return;
    }

    if ((!it->second.readAsm.empty() && it->second.writeAsm.empty())
        || ((s.architecture == Architecture::RISCV32 || s.architecture == Architecture::RISCV64)
            && it->second.constraint == "{x0}")
        || (s.architecture == Architecture::LoongArch64 && it->second.constraint == "{r0}")
        || it->second.constraint == "{$0}") {
        psi::ErrorStream() << "'%" << reg.name << "' is read-only\n";
        return;
    }
    llvm::Type* type = widthToType(it->second.width, *s.context);
    if (value->getType() != type) {
        if (value->getType()->isIntegerTy() && type->isIntegerTy()) {
            value = value->getType()->getIntegerBitWidth() < type->getIntegerBitWidth()
                ? builder->CreateZExt(value, type)
                : builder->CreateTrunc(value, type);
        } else {
            psi::ErrorStream() << "can't write this value's type into '%" << reg.name << "'\n";
            return;
        }
    }

    auto* asmType = llvm::FunctionType::get(llvm::Type::getVoidTy(*s.context), { type }, false);
    std::string constraints = it->second.constraint;
    if (!it->second.writeAsm.empty()) {
        if (reg.name == "nzcv" || reg.name == "apsr") constraints += ",~{cc}";
        else if (reg.name == "lr" || reg.name == "ctr" || reg.name == "xer"
            || reg.name == "hi" || reg.name == "lo") constraints += ",~{" + reg.name + "}";
        constraints += ",~{memory}";
    }
    auto* asmFn = llvm::InlineAsm::get(asmType, it->second.writeAsm, constraints, true);
    builder->CreateCall(asmFn, { value });
}

} // namespace psi_codegen
