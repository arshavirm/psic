#include "codegen_state.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicsWebAssembly.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Verifier.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace psi_codegen {

llvm::Value* processValue(State& s, const ValueNode& value, llvm::IRBuilder<>* builder)
{
    switch (value.kind) {
    case ValueKind::Number:
        if (value.numberIsFloat) {
            return llvm::ConstantFP::get(s.llvmTypes["f32"], value.numberAsFloat);
        }
        return builder->getInt32(value.numberAsInt);
    case ValueKind::String:
        return builder->CreateGlobalStringPtr(value.stringValue);
    case ValueKind::Register: {
        RegisterAddress access = resolveRegisterAddress(s, value.registerValue, builder);
        if (!access.address) {
            return nullptr;
        }
        llvm::Type* llvmType = resolveType(s, access.typeNode);
        if (!llvmType) {
            return nullptr;
        }
        return builder->CreateLoad(llvmType, access.address);
    }
    case ValueKind::SpecialRegister:
        return processSpecialRegisterRead(s, value.specialRegisterValue, builder);
    case ValueKind::Bool:
        return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*s.context), value.boolValue ? 1 : 0);
    case ValueKind::Null:
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(*s.context, 0));
    case ValueKind::Array:
        psi::ErrorStream() << "array literals can only be used directly as a register's "
                              "initializer (e.g. 'i32* arr = [1, 2, 3];'), not as a general value\n";
        return nullptr;
    }
    return nullptr;
}

llvm::Value* coerceValue(llvm::Value* value, llvm::Type* targetType, llvm::IRBuilder<>* builder, const std::string& context, bool treatSourceAsUnsigned)
{
    if (!value || !targetType) {
        return nullptr;
    }
    if (value->getType() == targetType) {
        return value;
    }

    llvm::Type* fromType = value->getType();

    if (fromType->isIntegerTy() && targetType->isIntegerTy()) {
        unsigned fromBits = fromType->getIntegerBitWidth();
        unsigned toBits = targetType->getIntegerBitWidth();
        if (fromBits < toBits) {

            return treatSourceAsUnsigned ? builder->CreateZExt(value, targetType) : builder->CreateSExt(value, targetType);
        }
        return builder->CreateTrunc(value, targetType);
    }

    if (fromType->isFloatingPointTy() && targetType->isFloatingPointTy()) {
        if (fromType->getPrimitiveSizeInBits() < targetType->getPrimitiveSizeInBits()) {
            return builder->CreateFPExt(value, targetType);
        }
        return builder->CreateFPTrunc(value, targetType);
    }

    if (fromType->isPointerTy() && targetType->isPointerTy()) {
        return value;
    }

    psi::ErrorStream() << "type mismatch in " << context << ": can't use this value here";
    return nullptr;
}

namespace {

llvm::Value* buildLocalArrayLiteral(State& s, const ValueNode& arrayValue, const TypeNode& declaredType, llvm::IRBuilder<>* builder)
{
    if (declaredType.pointerLevel < 1) {
        psi::ErrorStream() << "array initializer for '" << declaredType.baseName
                           << "' needs a pointer type, e.g. " << declaredType.baseName << "*\n";
        return nullptr;
    }

    TypeNode elementTypeNode = declaredType;
    elementTypeNode.pointerLevel -= 1;
    llvm::Type* elemType = resolveType(s, elementTypeNode);
    if (!elemType) {
        return nullptr;
    }

    std::vector<llvm::Value*> elements;
    for (auto* elementNode : arrayValue.arrayValues) {
        llvm::Value* v = processValue(s, *elementNode, builder);
        if (!v) {
            return nullptr;
        }
        v = coerceValue(v, elemType, builder, "array literal element", isUnsignedTypeName(elementTypeNode.baseName));
        if (!v) {
            return nullptr;
        }
        elements.push_back(v);
    }

    auto* arrayType = llvm::ArrayType::get(elemType, elements.size());
    auto* arrayAlloca = builder->CreateAlloca(arrayType, nullptr, "arrlit");
    for (size_t i = 0; i < elements.size(); i++) {
        llvm::Value* elementPtr = builder->CreateConstGEP2_32(arrayType, arrayAlloca, 0, (unsigned)i);
        builder->CreateStore(elements[i], elementPtr);
    }

    return arrayAlloca;
}

void predeclareLabels(State& s, const std::vector<CommandNode>& commands, llvm::Function* function)
{
    for (const auto& command : commands) {
        if (command.isEmpty || command.targetKind != TargetKind::None || !command.hasInstruction) {
            continue;
        }
        if (command.instruction.isSpecial || command.instruction.name != "label") {
            continue;
        }
        const std::string& labelName = command.values[0]->registerValue.name;
        if (!s.labels.count(labelName)) {
            s.labels[labelName] = llvm::BasicBlock::Create(*s.context, labelName, function);
        }
    }
}

llvm::Value* processCallInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.empty() || command.values[0]->kind != ValueKind::Register) {
        psi::ErrorStream() << "'call' needs a function name as its first operand\n";
        return nullptr;
    }

    const std::string& calleeName = command.values[0]->registerValue.name;
    auto fnIt = s.functionDeclarations.find(calleeName);
    if (fnIt == s.functionDeclarations.end()) {
        psi::ErrorStream() << "call to undefined function '" << calleeName << "'\n";
        return nullptr;
    }

    llvm::Function* callee = fnIt->second;
    size_t expectedArgCount = callee->arg_size();
    size_t givenArgCount = command.values.size() - 1;
    if (givenArgCount != expectedArgCount) {
        psi::ErrorStream() << "call to '" << calleeName << "' passes " << givenArgCount
                           << " argument(s), but it takes " << expectedArgCount;
        return nullptr;
    }

    auto paramTypesIt = s.functionParamTypes.find(calleeName);

    std::vector<llvm::Value*> args;
    for (size_t i = 1; i < command.values.size(); i++) {
        llvm::Value* arg = processValue(s, *command.values[i], builder);
        if (!arg) {
            return nullptr;
        }
        llvm::Type* paramType = callee->getFunctionType()->getParamType(i - 1);
        bool paramIsUnsigned = paramTypesIt != s.functionParamTypes.end()
            && isUnsignedTypeName(paramTypesIt->second[i - 1].baseName);
        arg = coerceValue(arg, paramType, builder, "argument " + std::to_string(i) + " to '" + calleeName + "'", paramIsUnsigned);
        if (!arg) {
            return nullptr;
        }
        args.push_back(arg);
    }

    return builder->CreateCall(callee, args);
}

llvm::Value* processRefInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.empty() || command.values[0]->kind != ValueKind::Register) {
        psi::ErrorStream() << "'ref' needs a register operand\n";
        return nullptr;
    }
    RegisterAddress access = resolveRegisterAddress(s, command.values[0]->registerValue, builder);
    return access.address;
}

llvm::Value* processSyscallInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (s.os != OperatingSystem::Linux) {
        psi::ErrorStream() << "'#syscall' currently supports only the Linux syscall ABI\n";

        return nullptr;
    }

    if (command.values.empty()) {
        psi::ErrorStream() << "'#syscall' needs at least a syscall number\n";
        return nullptr;
    }

    const size_t argCount = command.values.size() - 1;
    if (argCount > 6) {
        psi::ErrorStream() << "'#syscall' supports at most 6 arguments (got "
                           << argCount << ")\n";
        return nullptr;
    }

    const bool wide = s.architecture == Architecture::X86_64 || isAArch64(s.architecture)
        || s.architecture == Architecture::RISCV64;

    llvm::Type* wordType = wide
        ? llvm::Type::getInt64Ty(*s.context)
        : llvm::Type::getInt32Ty(*s.context);

    std::vector<llvm::Value*> operands;
    std::vector<llvm::Type*> operandTypes;

    for (auto* v : command.values) {
        llvm::Value* val = processValue(s, *v, builder);
        if (!val)
            return nullptr;

        if (val->getType() != wordType) {
            if (!val->getType()->isIntegerTy()) {
                psi::ErrorStream() << "'#syscall' operands must be integers\n";
                return nullptr;
            }

            const unsigned fromBits = val->getType()->getIntegerBitWidth();
            const unsigned toBits = wordType->getIntegerBitWidth();

            if (fromBits < toBits)
                val = builder->CreateZExt(val, wordType);
            else if (fromBits > toBits)
                val = builder->CreateTrunc(val, wordType);
        }

        operands.push_back(val);
        operandTypes.push_back(wordType);
    }

    // Per-architecture syscall ABI: the output register, then the register
    // sequence for the syscall number and up to 6 arguments, then clobbers.
    // On most targets the number register doubles as the result register.
    struct SyscallABI {
        Architecture arch;
        const char* output;
        std::initializer_list<const char*> operandRegs;
        const char* clobbers;
        const char* asmText;
    };
    static const SyscallABI abis[] = {
        {Architecture::X86_64, "{rax}", {"{rax}", "{rdi}", "{rsi}", "{rdx}", "{r10}", "{r8}", "{r9}"}, ",~{rcx},~{r11},~{memory}", "syscall"},
        {Architecture::X86, "{eax}", {"{eax}", "{ebx}", "{ecx}", "{edx}", "{esi}", "{edi}", "{ebp}"}, ",~{memory}", "int $$0x80"},
        {Architecture::AArch64, "{x0}", {"{x8}", "{x0}", "{x1}", "{x2}", "{x3}", "{x4}", "{x5}"}, ",~{x8},~{memory}", "svc #0"},
        {Architecture::ARM, "{r0}", {"{r7}", "{r0}", "{r1}", "{r2}", "{r3}", "{r4}", "{r5}"}, ",~{r7},~{memory}", "svc #0"},
        {Architecture::RISCV32, "{x10}", {"{x17}", "{x10}", "{x11}", "{x12}", "{x13}", "{x14}", "{x15}"}, ",~{memory}", "ecall"},
        {Architecture::RISCV64, "{x10}", {"{x17}", "{x10}", "{x11}", "{x12}", "{x13}", "{x14}", "{x15}"}, ",~{memory}", "ecall"},
    };

    const SyscallABI* abi = nullptr;
    for (const auto& candidate : abis)
        if (candidate.arch == s.architecture)
            abi = &candidate;
    if (!abi) {
        psi::ErrorStream() << "'#syscall' is not implemented for this architecture\n";
        return nullptr;
    }

    std::string constraints = "=" + std::string(abi->output);
    for (std::size_t i = 0; i < operands.size(); ++i)
        constraints += "," + std::string(abi->operandRegs.begin()[i]);

    constraints += abi->clobbers;

    auto* asmType = llvm::FunctionType::get(wordType, operandTypes, false);
    auto* asmFn = llvm::InlineAsm::get(asmType, abi->asmText, constraints, true);

    return builder->CreateCall(asmFn, operands);
}

llvm::Value* processFloatArithmeticInstruction(State& s,
    const std::string& op,
    const CommandNode& command,
    llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#" << op << "' needs exactly 2 operands\n";
        return nullptr;
    }

    llvm::Value* a = processValue(s, *command.values[0], builder);
    llvm::Value* b = processValue(s, *command.values[1], builder);
    if (!a || !b)
        return nullptr;

    llvm::Type* f32 = llvm::Type::getFloatTy(*s.context);
    llvm::Type* f64 = llvm::Type::getDoubleTy(*s.context);

    if (!((a->getType() == f32 && b->getType() == f32)
        || (a->getType() == f64 && b->getType() == f64))) {
        psi::ErrorStream() << "'#" << op
                           << "' needs two f32 operands or two f64 operands (not mixed)\n";
        return nullptr;
    }

    // LLVM selects the target's scalar FP instruction (or a soft-float helper).
    // This also avoids tying the language operation to a target's asm operand syntax.
    if (op == "fadd") return builder->CreateFAdd(a, b);
    if (op == "fsub") return builder->CreateFSub(a, b);
    if (op == "fmul") return builder->CreateFMul(a, b);
    return builder->CreateFDiv(a, b);
}

llvm::Value* processUnaryIntegerIntrinsic(State& s,
    const std::string& name,
    llvm::Value* value,
    llvm::IRBuilder<>* builder)
{
    if (!value || !value->getType()->isIntegerTy()) {
        psi::ErrorStream() << "'#" << name
                           << "' requires an integer operand\n";
        return nullptr;
    }

    llvm::Module* module = builder->GetInsertBlock()->getModule();
    llvm::Type* type = value->getType();
    llvm::Intrinsic::ID intrinsic;

    if (name == "clz")
        intrinsic = llvm::Intrinsic::ctlz;
    else if (name == "ctz")
        intrinsic = llvm::Intrinsic::cttz;
    else if (name == "popcnt")
        intrinsic = llvm::Intrinsic::ctpop;
    else if (name == "bswap")
        intrinsic = llvm::Intrinsic::bswap;
    else
        return nullptr;

    llvm::Function* fn = llvm::Intrinsic::getDeclaration(module, intrinsic, { type });

    if (intrinsic == llvm::Intrinsic::ctlz || intrinsic == llvm::Intrinsic::cttz) {
        return builder->CreateCall(fn, { value, llvm::ConstantInt::getFalse(*s.context) });
    }

    return builder->CreateCall(fn, { value });
}

llvm::Value* processVectorArithmeticInstruction(State& s,
    const std::string& name,
    const CommandNode& command,
    llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#" << name << "' needs exactly 2 operands\n";
        return nullptr;
    }

    llvm::Value* a = processValue(s, *command.values[0], builder);
    llvm::Value* b = processValue(s, *command.values[1], builder);
    if (!a || !b) {
        return nullptr;
    }

    if (!a->getType()->isVectorTy()) {
        psi::ErrorStream() << "'#" << name
                           << "' needs SIMD vector operands (e.g. f32x4, i32x4)\n";
        return nullptr;
    }
    if (a->getType() != b->getType()) {
        psi::ErrorStream() << "'#" << name << "' needs two operands of the same SIMD vector type\n";
        return nullptr;
    }

    bool isFloat = a->getType()->isFPOrFPVectorTy();

    if (name == "vadd")
        return isFloat ? builder->CreateFAdd(a, b) : builder->CreateAdd(a, b);
    if (name == "vsub")
        return isFloat ? builder->CreateFSub(a, b) : builder->CreateSub(a, b);
    if (name == "vmul")
        return isFloat ? builder->CreateFMul(a, b) : builder->CreateMul(a, b);
    if (name == "vdiv") {
        if (!isFloat) {
            psi::ErrorStream() << "'#vdiv' needs a floating-point SIMD vector (f32xN/f64xN) - "
                                  "use '#vdivi' or '#vdivu' for integer lanes\n";
            return nullptr;
        }
        return builder->CreateFDiv(a, b);
    }
    if (name == "vdivi" || name == "vdivu") {
        if (isFloat) {
            psi::ErrorStream() << "'#" << name << "' needs an integer SIMD vector - use '#vdiv' for f32xN/f64xN\n";
            return nullptr;
        }
        return name == "vdivi" ? builder->CreateSDiv(a, b) : builder->CreateUDiv(a, b);
    }
    if (name == "vand")
        return builder->CreateAnd(a, b);
    if (name == "vor")
        return builder->CreateOr(a, b);
    if (name == "vxor")
        return builder->CreateXor(a, b);
    if (name == "vmin" || name == "vmax") {
        llvm::Intrinsic::ID id = isFloat
            ? (name == "vmin" ? llvm::Intrinsic::minnum : llvm::Intrinsic::maxnum)
            : (name == "vmin" ? llvm::Intrinsic::smin : llvm::Intrinsic::smax);
        llvm::Function* fn = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(), id, { a->getType() });
        return builder->CreateCall(fn, { a, b });
    }

    return nullptr;
}

llvm::Value* processVectorFmaInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 3) {
        psi::ErrorStream() << "'#vfma' needs exactly 3 operands: a, b, c (computes a*b + c)\n";
        return nullptr;
    }

    llvm::Value* a = processValue(s, *command.values[0], builder);
    llvm::Value* b = processValue(s, *command.values[1], builder);
    llvm::Value* c = processValue(s, *command.values[2], builder);
    if (!a || !b || !c) {
        return nullptr;
    }

    if (!a->getType()->isVectorTy() || a->getType() != b->getType() || a->getType() != c->getType()) {
        psi::ErrorStream() << "'#vfma' needs three operands of the same SIMD vector type\n";
        return nullptr;
    }
    if (!a->getType()->isFPOrFPVectorTy()) {
        psi::ErrorStream() << "'#vfma' needs a floating-point SIMD vector (f32xN/f64xN)\n";
        return nullptr;
    }

    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
        builder->GetInsertBlock()->getModule(), llvm::Intrinsic::fma, { a->getType() });
    return builder->CreateCall(fn, { a, b, c });
}

llvm::Value* processVectorSplatInstruction(State& s,
    const CommandNode& command,
    llvm::IRBuilder<>* builder,
    const TypeNode* declaredType)
{
    if (!declaredType) {
        psi::ErrorStream() << "'#vsplat' needs a declared SIMD vector destination type "
                              "(e.g. 'f32x4 v = #vsplat x;')\n";
        return nullptr;
    }
    if (command.values.size() != 1) {
        psi::ErrorStream() << "'#vsplat' needs exactly 1 operand\n";
        return nullptr;
    }

    llvm::Type* vecType = resolveType(s, *declaredType);
    if (!vecType) {
        return nullptr;
    }
    if (!vecType->isVectorTy()) {
        psi::ErrorStream() << "'#vsplat' needs a SIMD vector destination type (e.g. f32x4, i32x4)\n";
        return nullptr;
    }

    llvm::Value* scalar = processValue(s, *command.values[0], builder);
    if (!scalar) {
        return nullptr;
    }

    llvm::Type* laneType = llvm::cast<llvm::VectorType>(vecType)->getElementType();
    scalar = coerceValue(scalar, laneType, builder, "'#vsplat' operand", isDeclaredUnsigned(s, *command.values[0]));
    if (!scalar) {
        return nullptr;
    }

    unsigned numLanes = llvm::cast<llvm::FixedVectorType>(vecType)->getNumElements();
    return builder->CreateVectorSplat(numLanes, scalar);
}

llvm::Value* processAtomicRMWInstruction(State& s,
    const std::string& name,
    llvm::AtomicRMWInst::BinOp op,
    const CommandNode& command,
    llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#" << name << "' needs exactly 2 operands (pointer, value)\n";
        return nullptr;
    }

    llvm::Value* ptr = processValue(s, *command.values[0], builder);
    llvm::Value* val = processValue(s, *command.values[1], builder);
    if (!ptr || !val) {
        return nullptr;
    }

    if (!ptr->getType()->isPointerTy()) {
        psi::ErrorStream() << "'#" << name << "' needs a pointer as its first operand\n";
        return nullptr;
    }

    bool isFloatOp = op == llvm::AtomicRMWInst::FAdd || op == llvm::AtomicRMWInst::FSub
        || op == llvm::AtomicRMWInst::FMax || op == llvm::AtomicRMWInst::FMin;
    llvm::Type* valType = val->getType();
    if (isFloatOp && !valType->isFloatingPointTy()) {
        psi::ErrorStream() << "'#" << name << "' needs a floating-point value operand\n";
        return nullptr;
    }
    if (!isFloatOp && !valType->isIntegerTy()) {
        psi::ErrorStream() << "'#" << name << "' needs an integer value operand\n";
        return nullptr;
    }

    llvm::Align align(valType->getPrimitiveSizeInBits() / 8);
    return builder->CreateAtomicRMW(op, ptr, val, align, llvm::AtomicOrdering::SequentiallyConsistent);
}

llvm::Value* processAtomicLoadInstruction(State& s,
    const CommandNode& command,
    llvm::IRBuilder<>* builder,
    const TypeNode* declaredType)
{
    if (!declaredType) {
        psi::ErrorStream() << "'#atomicload' needs a declared destination type (e.g. 'i32 x = #atomicload p;')\n";
        return nullptr;
    }
    if (command.values.size() != 1) {
        psi::ErrorStream() << "'#atomicload' needs exactly 1 operand (a pointer)\n";
        return nullptr;
    }

    llvm::Type* loadType = resolveType(s, *declaredType);
    if (!loadType) {
        return nullptr;
    }

    llvm::Value* ptr = processValue(s, *command.values[0], builder);
    if (!ptr) {
        return nullptr;
    }
    if (!ptr->getType()->isPointerTy()) {
        psi::ErrorStream() << "'#atomicload' needs a pointer operand\n";
        return nullptr;
    }

    llvm::LoadInst* load = builder->CreateLoad(loadType, ptr);
    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::SyncScope::SingleThread);
    return load;
}

llvm::Value* processAtomicStoreInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#atomicstore' needs exactly 2 operands (pointer, value)\n";
        return nullptr;
    }

    llvm::Value* ptr = processValue(s, *command.values[0], builder);
    llvm::Value* val = processValue(s, *command.values[1], builder);
    if (!ptr || !val) {
        return nullptr;
    }
    if (!ptr->getType()->isPointerTy()) {
        psi::ErrorStream() << "'#atomicstore' needs a pointer as its first operand\n";
        return nullptr;
    }

    llvm::StoreInst* store = builder->CreateStore(val, ptr);
    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::SyncScope::SingleThread);
    return nullptr;
}

llvm::Value* processAtomicCasInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 3) {
        psi::ErrorStream() << "'#cas' needs exactly 3 operands (pointer, expected, desired)\n";
        return nullptr;
    }

    llvm::Value* ptr = processValue(s, *command.values[0], builder);
    llvm::Value* expected = processValue(s, *command.values[1], builder);
    llvm::Value* desired = processValue(s, *command.values[2], builder);
    if (!ptr || !expected || !desired) {
        return nullptr;
    }
    if (!ptr->getType()->isPointerTy()) {
        psi::ErrorStream() << "'#cas' needs a pointer as its first operand\n";
        return nullptr;
    }

    desired = coerceValue(desired, expected->getType(), builder, "'#cas' desired-value operand");
    if (!desired) {
        return nullptr;
    }

    llvm::Align align(expected->getType()->getPrimitiveSizeInBits() / 8);
    llvm::AtomicCmpXchgInst* cmpxchg = builder->CreateAtomicCmpXchg(
        ptr, expected, desired, align,
        llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::AtomicOrdering::SequentiallyConsistent);

    return builder->CreateExtractValue(cmpxchg, 0);
}

enum class NativeInstructionResult {
    NotHandled,
    Handled,
};

NativeInstructionResult processNativeInstruction(State& s, const std::string& name,
    const CommandNode& command, llvm::IRBuilder<>* builder)
{
    const bool x86 = s.architecture == Architecture::X86 || s.architecture == Architecture::X86_64;
    const bool riscv = s.architecture == Architecture::RISCV32 || s.architecture == Architecture::RISCV64;
    const bool ppc = s.architecture == Architecture::PPC32 || s.architecture == Architecture::PPC64
        || s.architecture == Architecture::PPC64LE;
    const bool mips = s.architecture == Architecture::MIPS || s.architecture == Architecture::MIPSEL
        || s.architecture == Architecture::MIPS64 || s.architecture == Architecture::MIPS64EL;

    std::string assembly;
    bool memoryBarrier = false;
    if (name == "nop") {
        if (isWasm(s)) {
            if (!command.values.empty()) psi::ErrorStream() << "'#nop' takes no operands\n";
            return NativeInstructionResult::Handled;
        }
        assembly = s.architecture == Architecture::SystemZ ? "bcr 0, 0" : "nop";
    }
    if (x86 && (name == "lfence" || name == "sfence" || name == "mfence")) {
        assembly = name;
        memoryBarrier = true;
    }
    if (isARM(s.architecture)) {
        if (name == "dmb" || name == "dsb" || name == "isb") {
            assembly = name + " sy";
            memoryBarrier = true;
        }
        if (name == "wfi" || name == "wfe" || name == "sev" || name == "sevl") {
            if (name != "sevl" || isAArch64(s.architecture)) assembly = name;
        }
    }
    if (riscv) {
        if (name == "ecall" || name == "ebreak" || name == "wfi") {
            assembly = name;
            memoryBarrier = true;
        }
        if (name == "fence_i") {
            assembly = "fence.i";
            memoryBarrier = true;
        }
    }
    if (ppc && (name == "sync" || name == "lwsync" || name == "isync" || name == "eieio")) {
        assembly = name;
        memoryBarrier = true;
    }
    if (mips && name == "sync") {
        assembly = "sync";
        memoryBarrier = true;
    }
    if (s.architecture == Architecture::LoongArch64 && (name == "dbar" || name == "ibar")) {
        assembly = name + " 0";
        memoryBarrier = true;
    }
    if (s.architecture == Architecture::SystemZ && name == "serialize") {
        assembly = "bcr 15, 0";
        memoryBarrier = true;
    }
    if (assembly.empty()) return NativeInstructionResult::NotHandled;
    if (!command.values.empty()) {
        psi::ErrorStream() << "'#" << name << "' takes no operands\n";
        return NativeInstructionResult::Handled;
    }

    auto* functionType = llvm::FunctionType::get(builder->getVoidTy(), false);
    auto* assemblyFunction = llvm::InlineAsm::get(functionType, assembly,
        memoryBarrier ? "~{memory}" : "", true);
    builder->CreateCall(assemblyFunction);
    return NativeInstructionResult::Handled;
}

llvm::Value* processSpecialInstruction(State& s,
    const CommandNode& command,
    llvm::IRBuilder<>* builder,
    const TypeNode* declaredType)
{
    std::string name = command.instruction.name;
    std::replace(name.begin(), name.end(), '.', '_');

    if (name == "ptrcast" || name == "ptrtoint" || name == "inttoptr") {
        if (!declaredType || command.values.size() != 1) {
            psi::ErrorStream() << "'#" << name
                               << "' requires one operand and an explicitly typed result\n";
            return nullptr;
        }
        llvm::Value* operand = processValue(s, *command.values[0], builder);
        llvm::Type* resultType = resolveType(s, *declaredType);
        if (!operand || !resultType) return nullptr;
        llvm::Type* sourceType = operand->getType();
        if (name == "ptrcast") {
            if (!sourceType->isPointerTy() || !resultType->isPointerTy()) {
                psi::ErrorStream() << "'#ptrcast' requires pointer source and result types\n";
                return nullptr;
            }
            return operand;
        }
        if (name == "ptrtoint") {
            if (!sourceType->isPointerTy() || !resultType->isIntegerTy()) {
                psi::ErrorStream() << "'#ptrtoint' requires a pointer operand and integer result\n";
                return nullptr;
            }
            const auto& layout = builder->GetInsertBlock()->getModule()->getDataLayout();
            auto* pointerType = llvm::cast<llvm::PointerType>(sourceType);
            if (layout.isNonIntegralPointerType(pointerType)
                || layout.getPointerSizeInBits(pointerType->getAddressSpace())
                    != resultType->getIntegerBitWidth()) {
                psi::ErrorStream() << "'#ptrtoint' result width must match an integral pointer representation\n";
                return nullptr;
            }
            return builder->CreatePtrToInt(operand, resultType);
        }
        if (!sourceType->isIntegerTy() || !resultType->isPointerTy()) {
            psi::ErrorStream() << "'#inttoptr' requires an integer operand and pointer result\n";
            return nullptr;
        }
        const auto& layout = builder->GetInsertBlock()->getModule()->getDataLayout();
        auto* pointerType = llvm::cast<llvm::PointerType>(resultType);
        if (layout.isNonIntegralPointerType(pointerType)
            || layout.getPointerSizeInBits(pointerType->getAddressSpace())
                != sourceType->getIntegerBitWidth()) {
            psi::ErrorStream() << "'#inttoptr' operand width must match an integral pointer representation\n";
            return nullptr;
        }
        return builder->CreateIntToPtr(operand, resultType);
    }

    if (name == "memory_size" || name == "memory_grow") {
        if (!isWasm(s)) {
            psi::ErrorStream() << "'#" << name << "' requires WebAssembly\n";
            return nullptr;
        }
        if (command.values.size() != (name == "memory_size" ? 0u : 1u)) {
            psi::ErrorStream() << "'#" << name << "' has an incorrect operand count\n";
            return nullptr;
        }
        if (name == "memory_size") return wasmMemorySize(s, builder);
        auto* amount = processValue(s, *command.values[0], builder);
        if (!amount || !amount->getType()->isIntegerTy()) {
            psi::ErrorStream() << "'#memory_grow' requires an integer page count\n";
            return nullptr;
        }
        auto* word = builder->getIntNTy(s.architecture == Architecture::WASM64 ? 64 : 32);
        amount = builder->CreateZExtOrTrunc(amount, word);
        auto* fn = llvm::Intrinsic::getDeclaration(builder->GetInsertBlock()->getModule(),
            llvm::Intrinsic::wasm_memory_grow, {word});
        return builder->CreateCall(fn, {builder->getInt32(0), amount});
    }

    const bool x86 = s.architecture == Architecture::X86 || s.architecture == Architecture::X86_64;
    const NativeInstructionResult nativeResult = processNativeInstruction(s, name, command, builder);
    if (nativeResult == NativeInstructionResult::Handled) return nullptr;

    if (name == "syscall")
        return processSyscallInstruction(s, command, builder);

    if (name == "clz" || name == "ctz" || name == "popcnt" || name == "bswap") {
        if (command.values.size() != 1) {
            psi::ErrorStream() << "'#" << name
                               << "' needs exactly 1 operand\n";
            return nullptr;
        }

        return processUnaryIntegerIntrinsic(
            s, name, processValue(s, *command.values[0], builder), builder);
    }

    if (name == "fence") {
        if (!command.values.empty()) {
            psi::ErrorStream() << "'#fence' takes no operands\n";
            return nullptr;
        }

        builder->CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return nullptr;
    }

    if (name == "trap") {
        if (!command.values.empty()) {
            psi::ErrorStream() << "'#trap' takes no operands\n";
            return nullptr;
        }

        llvm::Function* trap = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(),
            llvm::Intrinsic::trap);

        builder->CreateCall(trap);
        return nullptr;
    }

    if (name == "pause" || name == "yield") {
        if (!command.values.empty()) {
            psi::ErrorStream() << "'#" << name
                               << "' takes no operands\n";
            return nullptr;
        }

        if (name == "pause" && !x86) {
            psi::ErrorStream() << "'#pause' is only available on x86 targets\n";
            return nullptr;
        }

        if (name == "yield" && !isARM(s.architecture)) {
            psi::ErrorStream() << "'#yield' is only available on ARM targets\n";
            return nullptr;
        }

        auto* voidType = llvm::Type::getVoidTy(*s.context);
        auto* asmType = llvm::FunctionType::get(voidType, false);
        const char* asmText = name == "pause" ? "pause" : "yield";

        auto* asmFn = llvm::InlineAsm::get(
            asmType, asmText, "", true);

        builder->CreateCall(asmFn);
        return nullptr;
    }

    if (name == "rdtsc") {
        if (!command.values.empty()) {
            psi::ErrorStream() << "'#rdtsc' takes no operands\n";
            return nullptr;
        }
        if (!x86) {
            psi::ErrorStream() << "'#rdtsc' is only available on x86 targets\n";
            return nullptr;
        }

        llvm::Function* fn = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(),
            llvm::Intrinsic::readcyclecounter);

        return builder->CreateCall(fn);
    }

    if (name == "fadd" || name == "fsub" || name == "fmul" || name == "fdiv") {
        return processFloatArithmeticInstruction(s, name, command, builder);
    }

    if (name == "vadd" || name == "vsub" || name == "vmul" || name == "vdiv"
        || name == "vdivi" || name == "vdivu" || name == "vand" || name == "vor"
        || name == "vxor" || name == "vmin" || name == "vmax") {
        return processVectorArithmeticInstruction(s, name, command, builder);
    }

    if (name == "vfma") {
        return processVectorFmaInstruction(s, command, builder);
    }

    if (name == "vsplat") {
        return processVectorSplatInstruction(s, command, builder, declaredType);
    }

    if (name.rfind("atomic", 0) == 0) {
        static const std::unordered_map<std::string, llvm::AtomicRMWInst::BinOp> atomicOps = {
            {"atomicadd", llvm::AtomicRMWInst::Add},
            {"atomicsub", llvm::AtomicRMWInst::Sub},
            {"atomicand", llvm::AtomicRMWInst::And},
            {"atomicor", llvm::AtomicRMWInst::Or},
            {"atomicxor", llvm::AtomicRMWInst::Xor},
            {"atomicnand", llvm::AtomicRMWInst::Nand},
            {"atomicxchg", llvm::AtomicRMWInst::Xchg},
            {"atomicmax", llvm::AtomicRMWInst::Max},
            {"atomicmin", llvm::AtomicRMWInst::Min},
            {"atomicumax", llvm::AtomicRMWInst::UMax},
            {"atomicumin", llvm::AtomicRMWInst::UMin},
            {"atomicfadd", llvm::AtomicRMWInst::FAdd},
            {"atomicfsub", llvm::AtomicRMWInst::FSub},
        };
        auto it = atomicOps.find(name);
        if (it != atomicOps.end())
            return processAtomicRMWInstruction(s, name, it->second, command, builder);

        if (name == "atomicload")
            return processAtomicLoadInstruction(s, command, builder, declaredType);
        if (name == "atomicstore")
            return processAtomicStoreInstruction(s, command, builder);
    }

    if (name == "cas") {
        return processAtomicCasInstruction(s, command, builder);
    }

    psi::ErrorStream() << "unsupported special instruction '#"
                       << name << "'\n";
    return nullptr;
}

void processUntargetedInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    const std::string& instruction = command.instruction.name;
    if (instruction == "ret") {
        if (command.values.empty()) {
            builder->CreateRetVoid();
            return;
        }
        llvm::Value* value = processValue(s, *command.values[0], builder);
        if (!value) return;
        value = coerceValue(value, s.currentFunction->getReturnType(), builder, "'ret'",
            isUnsignedTypeName(s.currentFunctionReturnType.baseName));
        if (value) builder->CreateRet(value);
        return;
    }

    if (instruction == "store") {
        llvm::Value* pointer = processValue(s, *command.values[0], builder);
        llvm::Value* value = processValue(s, *command.values[1], builder);
        if (!pointer || !value) return;
        if (!pointer->getType()->isPointerTy() || value->getType()->isVoidTy()) {
            psi::logError("'store' requires a pointer and a value");
            return;
        }
        TypeNode pointerType;
        if (command.values[0]->kind == ValueKind::Register
            && resolveRegisterTypeNode(s, command.values[0]->registerValue, pointerType)) {
            if (pointerType.pointerLevel < 1) {
                psi::logError("'store' requires a pointer operand");
                return;
            }
            --pointerType.pointerLevel;
            TypeNode valueType;
            if (command.values[1]->kind == ValueKind::Register
                && resolveRegisterTypeNode(s, command.values[1]->registerValue, valueType)
                && (valueType.baseName != pointerType.baseName
                    || valueType.pointerLevel != pointerType.pointerLevel)) {
                psi::logError("'store' value type must match the pointer's pointee type");
                return;
            }
        }
        builder->CreateStore(value, pointer);
        return;
    }

    if (instruction == "jmp") {
        const std::string& target = command.values[0]->registerValue.name;
        auto label = s.labels.find(target);
        if (label == s.labels.end()) {
            psi::ErrorStream() << "jmp to undefined label '" << target << "'\n";
            return;
        }
        builder->CreateBr(label->second);
        return;
    }

    if (instruction == "cjmp") {
        const std::string& target = command.values[1]->registerValue.name;
        auto label = s.labels.find(target);
        if (label == s.labels.end()) {
            psi::ErrorStream() << "cjmp to undefined label '" << target << "'\n";
            return;
        }
        llvm::Value* condition = processValue(s, *command.values[0], builder);
        if (!condition) return;
        if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1)) {
            condition = builder->CreateICmpNE(condition, llvm::Constant::getNullValue(condition->getType()));
        } else if (!condition->getType()->isIntegerTy(1)) {
            psi::ErrorStream() << "'cjmp' needs an integer condition";
            return;
        }
        auto* fallthrough = llvm::BasicBlock::Create(*s.context, "", s.currentFunction);
        builder->CreateCondBr(condition, label->second, fallthrough);
        builder->SetInsertPoint(fallthrough);
        return;
    }

    if (instruction == "label") {
        const std::string& name = command.values[0]->registerValue.name;
        llvm::BasicBlock* block = s.labels.count(name)
            ? s.labels[name]
            : llvm::BasicBlock::Create(*s.context, name, s.currentFunction);
        s.labels[name] = block;
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(block);
        builder->SetInsertPoint(block);
        return;
    }

    if (instruction == "call") processCallInstruction(s, command, builder);
}

void processLocalDeclaration(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    llvm::Type* type = resolveType(s, command.declaredType);
    if (!type) return;

    const bool hasInitializer = command.hasInstruction || !command.values.empty();
    llvm::Value* value = nullptr;
    if (hasInitializer) {
        value = computeCommandValue(s, command, builder, &command.declaredType);
        if (!value) return;
        value = coerceValue(value, type, builder, "declaration of '" + command.targetRegister.name + "'",
            isUnsignedTypeName(command.declaredType.baseName));
        if (!value) return;
    }

    const std::string& name = command.targetRegister.name;
    auto existing = s.locals.find(name);
    if (existing != s.locals.end()) {
        if (hasInitializer) builder->CreateStore(value, existing->second);
        return;
    }

    auto* slot = builder->CreateAlloca(type, nullptr, name);
    if (command.declaredType.alignment > 0)
        slot->setAlignment(llvm::Align(command.declaredType.alignment));
    s.locals[name] = slot;
    s.localTypes[name] = command.declaredType;
    s.declaredLocalNames.push_back(name);
    if (hasInitializer) builder->CreateStore(value, slot);
}

void processRegisterAssignment(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    llvm::Value* value = computeCommandValue(s, command, builder, nullptr);
    if (!value) return;

    RegisterAddress access = resolveRegisterAddress(s, command.targetRegister, builder);
    if (!access.address) return;
    llvm::Type* targetType = resolveType(s, access.typeNode);
    if (!targetType) return;
    value = coerceValue(value, targetType, builder, "assignment to '" + command.targetRegister.name + "'",
        isUnsignedTypeName(access.typeNode.baseName));
    if (value) builder->CreateStore(value, access.address);
}

llvm::Value* processBinaryInstruction(State& s, const std::string& op,
    const CommandNode& command, llvm::IRBuilder<>* builder)
{
    llvm::Value* lhs = !command.values.empty() ? processValue(s, *command.values[0], builder) : nullptr;
    llvm::Value* rhs = command.values.size() > 1 ? processValue(s, *command.values[1], builder) : nullptr;
    if (!lhs || !rhs) return nullptr;

    const bool isUnsigned = isDeclaredUnsigned(s, *command.values[0]);
    rhs = coerceValue(rhs, lhs->getType(), builder, "'" + op + "'", isUnsigned);
    if (!rhs) return nullptr;

    const bool isFloat = lhs->getType()->isFloatingPointTy();
    const bool isInteger = lhs->getType()->isIntegerTy();
    if (!isInteger && !isFloat) {
        psi::logError("'" + op + "' requires scalar numeric operands");
        return nullptr;
    }
    if (!isInteger && (op == "and" || op == "or" || op == "xor" || op == "lsh"
        || op == "rsh" || op == "land" || op == "lor")) {
        psi::logError("'" + op + "' requires integer operands");
        return nullptr;
    }

    auto trapIf = [&](llvm::Value* condition) {
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* trapBlock = llvm::BasicBlock::Create(*s.context, "psi.trap", function);
        llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(*s.context, "psi.cont", function);
        builder->CreateCondBr(condition, trapBlock, continueBlock);
        builder->SetInsertPoint(trapBlock);
        llvm::Function* trap = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(), llvm::Intrinsic::trap);
        builder->CreateCall(trap);
        builder->CreateUnreachable();
        builder->SetInsertPoint(continueBlock);
    };

    if (op == "add") return isFloat ? builder->CreateFAdd(lhs, rhs) : builder->CreateAdd(lhs, rhs);
    if (op == "sub") return isFloat ? builder->CreateFSub(lhs, rhs) : builder->CreateSub(lhs, rhs);
    if (op == "mul") return isFloat ? builder->CreateFMul(lhs, rhs) : builder->CreateMul(lhs, rhs);
    if (op == "div") {
        if (isFloat) return builder->CreateFDiv(lhs, rhs);
        llvm::Value* invalid = builder->CreateICmpEQ(rhs,
            llvm::ConstantInt::get(rhs->getType(), 0));
        if (!isUnsigned) {
            const unsigned width = lhs->getType()->getIntegerBitWidth();
            const uint64_t minimumBits = uint64_t { 1 } << (width - 1);
            llvm::Value* isMinimum = builder->CreateICmpEQ(lhs,
                llvm::ConstantInt::get(lhs->getType(), minimumBits));
            llvm::Value* isNegativeOne = builder->CreateICmpEQ(rhs,
                llvm::ConstantInt::getAllOnesValue(rhs->getType()));
            invalid = builder->CreateOr(invalid,
                builder->CreateAnd(isMinimum, isNegativeOne));
        }
        trapIf(invalid);
        return isUnsigned ? builder->CreateUDiv(lhs, rhs) : builder->CreateSDiv(lhs, rhs);
    }
    if (op == "mod") {
        if (isFloat) return builder->CreateFRem(lhs, rhs);
        llvm::Value* invalid = builder->CreateICmpEQ(rhs,
            llvm::ConstantInt::get(rhs->getType(), 0));
        if (!isUnsigned) {
            const unsigned width = lhs->getType()->getIntegerBitWidth();
            const uint64_t minimumBits = uint64_t { 1 } << (width - 1);
            llvm::Value* isMinimum = builder->CreateICmpEQ(lhs,
                llvm::ConstantInt::get(lhs->getType(), minimumBits));
            llvm::Value* isNegativeOne = builder->CreateICmpEQ(rhs,
                llvm::ConstantInt::getAllOnesValue(rhs->getType()));
            invalid = builder->CreateOr(invalid,
                builder->CreateAnd(isMinimum, isNegativeOne));
        }
        trapIf(invalid);
        return isUnsigned ? builder->CreateURem(lhs, rhs) : builder->CreateSRem(lhs, rhs);
    }
    if (op == "eq") return isFloat ? builder->CreateFCmpOEQ(lhs, rhs) : builder->CreateICmpEQ(lhs, rhs);
    if (op == "neq") return isFloat ? builder->CreateFCmpONE(lhs, rhs) : builder->CreateICmpNE(lhs, rhs);
    if (op == "gt") {
        if (isFloat) return builder->CreateFCmpOGT(lhs, rhs);
        return isUnsigned ? builder->CreateICmpUGT(lhs, rhs) : builder->CreateICmpSGT(lhs, rhs);
    }
    if (op == "lt") {
        if (isFloat) return builder->CreateFCmpOLT(lhs, rhs);
        return isUnsigned ? builder->CreateICmpULT(lhs, rhs) : builder->CreateICmpSLT(lhs, rhs);
    }
    if (op == "gte") {
        if (isFloat) return builder->CreateFCmpOGE(lhs, rhs);
        return isUnsigned ? builder->CreateICmpUGE(lhs, rhs) : builder->CreateICmpSGE(lhs, rhs);
    }
    if (op == "lte") {
        if (isFloat) return builder->CreateFCmpOLE(lhs, rhs);
        return isUnsigned ? builder->CreateICmpULE(lhs, rhs) : builder->CreateICmpSLE(lhs, rhs);
    }
    if (op == "and") return builder->CreateAnd(lhs, rhs);
    if (op == "or") return builder->CreateOr(lhs, rhs);
    if (op == "xor") return builder->CreateXor(lhs, rhs);
    if (op == "lsh" || op == "rsh") {
        llvm::Value* tooLarge = builder->CreateICmpUGE(rhs,
            llvm::ConstantInt::get(rhs->getType(), lhs->getType()->getIntegerBitWidth()));
        trapIf(tooLarge);
        if (op == "lsh") return builder->CreateShl(lhs, rhs);
        return isUnsigned ? builder->CreateLShr(lhs, rhs) : builder->CreateAShr(lhs, rhs);
    }
    if (op == "land") {
        auto* lhsIsTrue = builder->CreateICmpNE(lhs, llvm::Constant::getNullValue(lhs->getType()));
        auto* rhsIsTrue = builder->CreateICmpNE(rhs, llvm::Constant::getNullValue(rhs->getType()));
        return builder->CreateAnd(lhsIsTrue, rhsIsTrue);
    }
    if (op == "lor") {
        auto* lhsIsTrue = builder->CreateICmpNE(lhs, llvm::Constant::getNullValue(lhs->getType()));
        auto* rhsIsTrue = builder->CreateICmpNE(rhs, llvm::Constant::getNullValue(rhs->getType()));
        return builder->CreateOr(lhsIsTrue, rhsIsTrue);
    }

    psi::ErrorStream() << "unsupported instruction '" << op << "'\n";
    return nullptr;
}

} // namespace

llvm::Value* computeCommandValue(State& s, const CommandNode& command, llvm::IRBuilder<>* builder,
    const TypeNode* declaredType)
{
    if (command.hasInstruction) {
        if (command.instruction.isSpecial) {
            return processSpecialInstruction(s, command, builder, declaredType);
        }

        const std::string& op = command.instruction.name;

        if (op == "call") {
            return processCallInstruction(s, command, builder);
        }
        if (op == "ref") {
            return processRefInstruction(s, command, builder);
        }
        if (op == "load") {
            auto* p = processValue(s, *command.values[0], builder);
            auto* type = declaredType ? resolveType(s, *declaredType) : nullptr;
            if (!p || !type) return nullptr;
            if (!p->getType()->isPointerTy()) {
                psi::logError("'load' requires a pointer operand");
                return nullptr;
            }
            TypeNode pointerType;
            if (command.values[0]->kind == ValueKind::Register
                && resolveRegisterTypeNode(s, command.values[0]->registerValue, pointerType)) {
                TypeNode expectedType = *declaredType;
                if (pointerType.pointerLevel < 1) {
                    psi::logError("'load' requires a pointer operand");
                    return nullptr;
                }
                --pointerType.pointerLevel;
                if (pointerType.baseName != expectedType.baseName
                    || pointerType.pointerLevel != expectedType.pointerLevel) {
                    psi::logError("'load' result type must match the pointer's pointee type");
                    return nullptr;
                }
            }
            return builder->CreateLoad(type, p);
        }
        if (op == "not") {
            auto* v = processValue(s, *command.values[0], builder);
            if (!v) return nullptr;
            if (!v->getType()->isIntOrIntVectorTy()) {
                psi::logError("'not' requires an integer operand");
                return nullptr;
            }
            return builder->CreateNot(v);
        }
        if (op == "lnot") {
            auto* v = processValue(s, *command.values[0], builder);
            if (!v) return nullptr;
            if (!v->getType()->isIntegerTy()) {
                psi::logError("'lnot' requires an integer operand");
                return nullptr;
            }
            return builder->CreateICmpEQ(v, llvm::Constant::getNullValue(v->getType()));
        }

        return processBinaryInstruction(s, op, command, builder);
    }

    if (declaredType && !command.values.empty() && command.values[0]->kind == ValueKind::Array) {
        return buildLocalArrayLiteral(s, *command.values[0], *declaredType, builder);
    }

    return processValue(s, *command.values[0], builder);
}

void processCommand(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.isEmpty) {
        return;
    }

    if (command.targetKind == TargetKind::SpecialRegister) {
        llvm::Value* value = computeCommandValue(s, command, builder, nullptr);
        if (!value) {
            return;
        }
        processSpecialRegisterWrite(s, command.targetSpecialRegister, value, builder);
        return;
    }

    if (command.targetKind == TargetKind::None) {
        if (!command.hasInstruction) return;
        if (command.instruction.isSpecial) {
            processSpecialInstruction(s, command, builder, nullptr);
            return;
        }
        processUntargetedInstruction(s, command, builder);
        return;
    }

    if (command.hasDeclaredType) processLocalDeclaration(s, command, builder);
    else processRegisterAssignment(s, command, builder);
}

void generateFunctionBody(State& s, const BlockNode& body, llvm::Function* function,
    const std::vector<ArgNode>* args, const std::string& diagnosticName)
{
    s.labels.clear();
    s.locals.clear();
    s.localTypes.clear();
    s.declaredLocalNames.clear();

    s.currentFunction = function;
    auto returnTypeIt = s.functionReturnTypes.find(diagnosticName);
    s.currentFunctionReturnType = (returnTypeIt != s.functionReturnTypes.end()) ? returnTypeIt->second : TypeNode { };

    llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(*s.context, "entry", function);
    llvm::IRBuilder<> builder(*s.context);
    builder.SetInsertPoint(entry_block);

    if (args) {
        size_t idx = 0;
        for (auto& arg : function->args()) {
            const ArgNode& argNode = (*args)[idx];
            llvm::Type* argType = arg.getType();
            auto* slot = builder.CreateAlloca(argType, nullptr, argNode.name);
            builder.CreateStore(&arg, slot);
            s.locals[argNode.name] = slot;
            s.localTypes[argNode.name] = argNode.type;
            idx++;
        }
    }

    predeclareLabels(s, body.commands, function);

    for (const CommandNode& command : body.commands) {
        if (psi::hadErrors()) break;
        const bool isLabel = command.hasInstruction && !command.instruction.isSpecial
            && command.instruction.name == "label";
        if (builder.GetInsertBlock()->getTerminator() && !isLabel) {
            if (!command.isEmpty) psi::logError("instruction after a terminator requires a label");
            continue;
        }
        processCommand(s, command, &builder);
    }

    if (!builder.GetInsertBlock()->getTerminator()) {
        if (function->getReturnType()->isVoidTy()) {
            builder.CreateRetVoid();
        } else {
            psi::ErrorStream() << "'" << diagnosticName
                               << "' falls off the end without a return\n";
            builder.CreateUnreachable();
        }
    }

    std::string errStr;
    llvm::raw_string_ostream errStream(errStr);
    if (llvm::verifyFunction(*function, &errStream)) {
        errStream.flush();
        psi::ErrorStream() << "function verification failed for '" << diagnosticName
                           << "':\n"
                           << errStr << "\n";
    }
}

} // namespace psi_codegen
