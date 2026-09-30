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
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace psi_codegen {

static llvm::Value* flushSubnormal(llvm::Value* value, llvm::IRBuilder<>* builder)
{
    llvm::Type* valueType = value->getType();
    auto* vectorType = llvm::dyn_cast<llvm::FixedVectorType>(valueType);
    llvm::Type* laneType = vectorType ? vectorType->getElementType() : valueType;
    if (!laneType->isFloatingPointTy()) return value;

    const unsigned laneBits = laneType->getPrimitiveSizeInBits();
    if (laneBits != 32 && laneBits != 64) return value;
    llvm::Type* integerLaneType = llvm::IntegerType::get(builder->getContext(), laneBits);
    llvm::Type* integerType = vectorType
        ? llvm::FixedVectorType::get(integerLaneType, vectorType->getNumElements())
        : integerLaneType;
    llvm::Value* bits = builder->CreateBitCast(value, integerType);

    const llvm::APInt exponentMask(laneBits,
        laneBits == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL);
    const llvm::APInt fractionMask(laneBits,
        laneBits == 32 ? 0x007fffffULL : 0x000fffffffffffffULL);
    const llvm::APInt signMask(laneBits,
        laneBits == 32 ? 0x80000000ULL : 0x8000000000000000ULL);
    auto integerConstant = [&](const llvm::APInt& constantBits) -> llvm::Value* {
        llvm::Constant* laneConstant = llvm::ConstantInt::get(integerLaneType, constantBits);
        if (!vectorType) return laneConstant;
        return builder->CreateVectorSplat(vectorType->getNumElements(), laneConstant);
    };

    llvm::Value* exponent = builder->CreateAnd(bits, integerConstant(exponentMask));
    llvm::Value* fraction = builder->CreateAnd(bits, integerConstant(fractionMask));
    llvm::Value* isSubnormal = builder->CreateAnd(
        builder->CreateICmpEQ(exponent, llvm::Constant::getNullValue(integerType)),
        builder->CreateICmpNE(fraction, llvm::Constant::getNullValue(integerType)));
    llvm::Value* signedZero = builder->CreateAnd(bits, integerConstant(signMask));
    llvm::Value* normalized = builder->CreateSelect(isSubnormal, signedZero, bits);
    return builder->CreateBitCast(normalized, valueType);
}

static llvm::Value* canonicalizeNaN(llvm::Value* value, llvm::IRBuilder<>* builder)
{
    value = flushSubnormal(value, builder);
    llvm::Type* valueType = value->getType();
    const bool isVector = valueType->isVectorTy();
    auto* vectorType = isVector ? llvm::dyn_cast<llvm::FixedVectorType>(valueType) : nullptr;
    llvm::Type* laneType = vectorType ? vectorType->getElementType() : valueType;
    if (!laneType->isFloatingPointTy()) return value;

    const unsigned laneBits = laneType->getPrimitiveSizeInBits();
    const bool isF32 = laneBits == 32;
    if (!isF32 && laneBits != 64) return value;
    llvm::Type* integerLaneType = llvm::IntegerType::get(builder->getContext(), laneBits);
    llvm::Type* integerType = vectorType
        ? llvm::FixedVectorType::get(integerLaneType, vectorType->getNumElements())
        : integerLaneType;
    llvm::Value* bits = builder->CreateBitCast(value, integerType);

    const llvm::APInt exponentMask(laneBits,
        isF32 ? 0x7f800000ULL : 0x7ff0000000000000ULL);
    const llvm::APInt fractionMask(laneBits,
        isF32 ? 0x007fffffULL : 0x000fffffffffffffULL);
    const llvm::APInt canonicalNaNBits = exponentMask | llvm::APInt(laneBits,
        isF32 ? 0x00400000ULL : 0x0008000000000000ULL);
    auto integerConstant = [&](const llvm::APInt& constantBits) -> llvm::Value* {
        llvm::Constant* laneConstant = llvm::ConstantInt::get(integerLaneType, constantBits);
        if (!vectorType) return laneConstant;
        return builder->CreateVectorSplat(vectorType->getNumElements(), laneConstant);
    };

    llvm::Value* exponent = builder->CreateAnd(bits, integerConstant(exponentMask));
    llvm::Value* fraction = builder->CreateAnd(bits, integerConstant(fractionMask));
    llvm::Value* isNaN = builder->CreateAnd(
        builder->CreateICmpEQ(exponent, integerConstant(exponentMask)),
        builder->CreateICmpNE(fraction, llvm::Constant::getNullValue(integerType)));
    llvm::Value* canonical = builder->CreateBitCast(
        integerConstant(canonicalNaNBits), valueType);
    return builder->CreateSelect(isNaN, canonical, value);
}

llvm::Value* processValue(State& s, const ValueNode& value, llvm::IRBuilder<>* builder)
{
    switch (value.kind) {
    case ValueKind::Number:
        if (value.numberIsFloat) {
            return llvm::ConstantFP::get(s.llvmTypes["f64"], value.numberAsFloat);
        }
        return builder->getInt64(value.numberAsInt);
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
        if (std::any_of(value.registerValue.accessors.begin(),
                value.registerValue.accessors.end(),
                [](const AccessorNode& accessor) { return accessor.kind == AccessorKind::Index; })) {
            trapIfNullPointer(access.address, builder);
            trapIfAccessRangeWraps(access.address, llvmType, builder);
        }
        return loadValueBytewiseAtomic(*builder, llvmType, access.address);
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
        value = flushSubnormal(value, builder);
        if (fromType->getPrimitiveSizeInBits() < targetType->getPrimitiveSizeInBits()) {
            return canonicalizeNaN(builder->CreateFPExt(value, targetType), builder);
        }
        return canonicalizeNaN(builder->CreateFPTrunc(value, targetType), builder);
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
    if (declaredType.pointerLevel < 1 && !declaredType.isView) {
        psi::ErrorStream() << "array initializer for '" << declaredType.baseName
                           << "' needs a pointer or bounded view type, e.g. "
                           << declaredType.baseName << "* or " << declaredType.baseName << "[]\n";
        return nullptr;
    }

    TypeNode elementTypeNode = declaredType;
    if (declaredType.isView) elementTypeNode.isView = false;
    else elementTypeNode.pointerLevel -= 1;
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
    // Allocate at the literal's execution point. A declaration inside a loop
    // creates fresh backing storage each time so copied views keep referring to
    // the previous execution's elements until this function returns.
    llvm::AllocaInst* arrayAlloca = builder->CreateAlloca(arrayType, nullptr,
        declaredType.isView ? "view.data" : "arrlit");
    for (size_t i = 0; i < elements.size(); i++) {
        llvm::Value* elementPtr = builder->CreateConstGEP2_32(arrayType, arrayAlloca, 0, (unsigned)i);
        storeValueWithZeroedPadding(*builder, elements[i], elementPtr);
    }

    if (declaredType.isView) {
        llvm::Type* viewType = resolveType(s, declaredType);
        llvm::Value* data = builder->CreateConstGEP2_32(arrayType, arrayAlloca, 0, 0);
        llvm::Value* view = llvm::UndefValue::get(viewType);
        view = builder->CreateInsertValue(view, data, {0});
        view = builder->CreateInsertValue(view,
            builder->getInt64(static_cast<std::uint64_t>(elements.size())), {1});
        return view;
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

llvm::Value* processCallInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder,
    const TypeNode* declaredType = nullptr)
{
    if (command.values.empty() || command.values[0]->kind != ValueKind::Register) {
        psi::ErrorStream() << "'call' needs a function name as its first operand\n";
        return nullptr;
    }

    const std::string& calleeName = command.values[0]->registerValue.name;
    auto fnIt = s.functionDeclarations.find(calleeName);
    if (fnIt == s.functionDeclarations.end()) {
        llvm::Value* calleeValue = processValue(s, *command.values[0], builder);
        if (!calleeValue || !calleeValue->getType()->isPointerTy()) {
            psi::ErrorStream() << "indirect call requires a function pointer operand\n";
            return nullptr;
        }
        trapIfNullPointer(calleeValue, builder);
        std::vector<llvm::Type*> parameterTypes;
        std::vector<llvm::Value*> args;
        for (std::size_t i = 1; i < command.values.size(); ++i) {
            auto* arg = processValue(s, *command.values[i], builder);
            if (!arg) return nullptr;
            parameterTypes.push_back(arg->getType());
            args.push_back(arg);
        }
        llvm::Type* returnType = declaredType ? resolveType(s, *declaredType) : builder->getVoidTy();
        if (!returnType) return nullptr;
        auto* signature = llvm::FunctionType::get(returnType, parameterTypes, false);
        return builder->CreateCall(signature, calleeValue, args);
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
    RegisterAddress access = resolveRegisterAddress(s, command.values[0]->registerValue,
        builder, false);
    return access.address;
}

llvm::Value* processSyscallInstruction(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (s.os != OperatingSystem::Linux) {
        psi::ErrorStream() << "'#syscall' currently supports only the Linux syscall ABI\n";

        return nullptr;
    }

    std::string targetTriple = s.targetTriple;
    std::transform(targetTriple.begin(), targetTriple.end(), targetTriple.begin(),
        [](unsigned char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a') : static_cast<char>(character);
        });
    if (s.architecture == Architecture::X86_64
        && targetTriple.find("gnux32") != std::string::npos) {
        psi::ErrorStream() << "'#syscall' is not available for the Linux x32 ABI\n";
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

    a = flushSubnormal(a, builder);
    b = flushSubnormal(b, builder);

    // LLVM selects the target's scalar FP instruction (or a soft-float helper).
    // This also avoids tying the language operation to a target's asm operand syntax.
    if (op == "fadd") return canonicalizeNaN(builder->CreateFAdd(a, b), builder);
    if (op == "fsub") return canonicalizeNaN(builder->CreateFSub(a, b), builder);
    if (op == "fmul") return canonicalizeNaN(builder->CreateFMul(a, b), builder);
    return canonicalizeNaN(builder->CreateFDiv(a, b), builder);
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
    else if (name == "bitreverse")
        intrinsic = llvm::Intrinsic::bitreverse;
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
    if (isFloat) {
        a = flushSubnormal(a, builder);
        b = flushSubnormal(b, builder);
    }

    if (name == "vadd")
        return isFloat ? canonicalizeNaN(builder->CreateFAdd(a, b), builder)
                       : builder->CreateAdd(a, b);
    if (name == "vsub")
        return isFloat ? canonicalizeNaN(builder->CreateFSub(a, b), builder)
                       : builder->CreateSub(a, b);
    if (name == "vmul")
        return isFloat ? canonicalizeNaN(builder->CreateFMul(a, b), builder)
                       : builder->CreateMul(a, b);
    if (name == "vdiv") {
        if (!isFloat) {
            psi::ErrorStream() << "'#vdiv' needs a floating-point SIMD vector (f32xN/f64xN) - "
                                  "use '#vdivi' or '#vdivu' for integer lanes\n";
            return nullptr;
        }
        return canonicalizeNaN(builder->CreateFDiv(a, b), builder);
    }
    if (name == "vdivi" || name == "vdivu") {
        if (isFloat) {
            psi::ErrorStream() << "'#" << name << "' needs an integer SIMD vector - use '#vdiv' for f32xN/f64xN\n";
            return nullptr;
        }
        const auto* vectorType = llvm::cast<llvm::FixedVectorType>(a->getType());
        const unsigned laneCount = vectorType->getNumElements();
        llvm::Type* laneType = vectorType->getElementType();
        llvm::Value* zero = llvm::Constant::getNullValue(a->getType());
        llvm::Value* invalidLanes = builder->CreateICmpEQ(b, zero);
        if (name == "vdivi") {
            const unsigned laneWidth = laneType->getIntegerBitWidth();
            const auto minValue = llvm::ConstantInt::get(laneType,
                llvm::APInt::getSignedMinValue(laneWidth));
            llvm::Value* minimum = builder->CreateVectorSplat(laneCount, minValue);
            llvm::Value* minusOne = llvm::Constant::getAllOnesValue(a->getType());
            llvm::Value* overflowLanes = builder->CreateAnd(
                builder->CreateICmpEQ(a, minimum),
                builder->CreateICmpEQ(b, minusOne));
            invalidLanes = builder->CreateOr(invalidLanes, overflowLanes);
        }

        // LLVM vector division is poison if any lane divides by zero or, for
        // signed division, overflows. Reduce the per-lane checks before
        // emitting the division so PSI's specified outcome is a trap.
        llvm::Value* invalid = llvm::ConstantInt::getFalse(*s.context);
        for (unsigned lane = 0; lane < laneCount; ++lane) {
            llvm::Value* laneInvalid = builder->CreateExtractElement(
                invalidLanes, builder->getInt32(lane));
            invalid = builder->CreateOr(invalid, laneInvalid);
        }

        trapIfCondition(invalid, "vector.div", builder);
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
        llvm::Value* result = builder->CreateCall(fn, { a, b });
        if (!isFloat) return result;

        // LLVM minnum/maxnum may choose either zero sign for equal operands.
        // PSI gives that edge a stable rule: min selects -0 if either input
        // is negative; max selects -0 only when both inputs are negative.
        const auto* floatingVector = llvm::cast<llvm::FixedVectorType>(a->getType());
        const unsigned laneCount = floatingVector->getNumElements();
        llvm::Type* laneType = floatingVector->getElementType();
        const unsigned laneBits = laneType->getPrimitiveSizeInBits();
        llvm::Type* integerLaneType = llvm::IntegerType::get(*s.context, laneBits);
        llvm::Type* integerVectorType = llvm::FixedVectorType::get(integerLaneType, laneCount);
        llvm::Value* integerA = builder->CreateBitCast(a, integerVectorType);
        llvm::Value* integerB = builder->CreateBitCast(b, integerVectorType);
        const llvm::APInt signBit = llvm::APInt(laneBits, 1).shl(laneBits - 1);
        llvm::Value* signMask = builder->CreateVectorSplat(laneCount,
            llvm::ConstantInt::get(integerLaneType, signBit));
        llvm::Value* signBits = name == "vmin"
            ? builder->CreateOr(integerA, integerB)
            : builder->CreateAnd(integerA, integerB);
        llvm::Value* zeroBits = builder->CreateAnd(signBits, signMask);
        llvm::Value* stableZero = builder->CreateBitCast(zeroBits, a->getType());
        llvm::Value* zero = llvm::Constant::getNullValue(a->getType());
        llvm::Value* bothZero = builder->CreateAnd(
            builder->CreateFCmpOEQ(a, zero), builder->CreateFCmpOEQ(b, zero));

        // Do not expose target-dependent NaN payload selection. A lone quiet
        // NaN yields the numeric operand; two NaNs or any signaling NaN yield
        // the canonical positive quiet NaN for the lane format.
        const bool isF32 = laneBits == 32;
        const llvm::APInt exponentMask = llvm::APInt(laneBits,
            isF32 ? 0x7f800000ULL : 0x7ff0000000000000ULL);
        const llvm::APInt fractionMask = llvm::APInt(laneBits,
            isF32 ? 0x007fffffULL : 0x000fffffffffffffULL);
        const llvm::APInt quietBit = llvm::APInt(laneBits,
            isF32 ? 0x00400000ULL : 0x0008000000000000ULL);
        const llvm::APInt canonicalNaNBits = exponentMask | quietBit;
        auto splatInteger = [&](const llvm::APInt& bits) {
            return builder->CreateVectorSplat(laneCount,
                llvm::ConstantInt::get(integerLaneType, bits));
        };
        llvm::Value* exponentVector = splatInteger(exponentMask);
        llvm::Value* fractionVector = splatInteger(fractionMask);
        llvm::Value* quietVector = splatInteger(quietBit);
        llvm::Value* canonicalNaN = splatInteger(canonicalNaNBits);
        auto isNaN = [&](llvm::Value* bits) {
            llvm::Value* allExponentBits = builder->CreateICmpEQ(
                builder->CreateAnd(bits, exponentVector), exponentVector);
            llvm::Value* hasFractionBits = builder->CreateICmpNE(
                builder->CreateAnd(bits, fractionVector),
                llvm::Constant::getNullValue(integerVectorType));
            return builder->CreateAnd(allExponentBits, hasFractionBits);
        };
        llvm::Value* nanA = isNaN(integerA);
        llvm::Value* nanB = isNaN(integerB);
        llvm::Value* signalingA = builder->CreateAnd(nanA,
            builder->CreateICmpEQ(builder->CreateAnd(integerA, quietVector),
                llvm::Constant::getNullValue(integerVectorType)));
        llvm::Value* signalingB = builder->CreateAnd(nanB,
            builder->CreateICmpEQ(builder->CreateAnd(integerB, quietVector),
                llvm::Constant::getNullValue(integerVectorType)));
        llvm::Value* anySignalingNaN = builder->CreateOr(signalingA, signalingB);
        llvm::Value* onlyNanA = builder->CreateAnd(nanA, builder->CreateNot(nanB));
        llvm::Value* onlyNanB = builder->CreateAnd(nanB, builder->CreateNot(nanA));
        llvm::Value* bothNaN = builder->CreateAnd(nanA, nanB);
        llvm::Value* anyNaN = builder->CreateOr(nanA, nanB);

        llvm::Value* nanResultBits = integerA;
        nanResultBits = builder->CreateSelect(onlyNanA, integerB, nanResultBits);
        nanResultBits = builder->CreateSelect(onlyNanB, integerA, nanResultBits);
        nanResultBits = builder->CreateSelect(bothNaN, canonicalNaN, nanResultBits);
        nanResultBits = builder->CreateSelect(anySignalingNaN,
            canonicalNaN, nanResultBits);
        llvm::Value* stableNaNResult = builder->CreateBitCast(nanResultBits, a->getType());
        result = builder->CreateSelect(anyNaN, stableNaNResult, result);
        return builder->CreateSelect(bothZero, stableZero, result);
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

    a = flushSubnormal(a, builder);
    b = flushSubnormal(b, builder);
    c = flushSubnormal(c, builder);

    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
        builder->GetInsertBlock()->getModule(), llvm::Intrinsic::fma, { a->getType() });
    return canonicalizeNaN(builder->CreateCall(fn, { a, b, c }), builder);
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

llvm::Value* processCanonicalAtomicFloatRmw(llvm::AtomicRMWInst::BinOp op,
    llvm::Value* pointer, llvm::Value* value, llvm::Align alignment,
    llvm::IRBuilder<>* builder)
{
    llvm::Type* floatType = value->getType();
    auto* bitsType = builder->getIntNTy(floatType->getPrimitiveSizeInBits());
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* preheader = builder->GetInsertBlock();
    auto* retryBlock = llvm::BasicBlock::Create(builder->getContext(),
        "atomic.float.retry", function);
    auto* doneBlock = llvm::BasicBlock::Create(builder->getContext(),
        "atomic.float.done", function);

    // The monotonic seed load is only a candidate expected value. The
    // successful sequentially consistent compare-exchange is the operation's
    // linearization point; failed retries do not publish a floating result.
    auto* initial = builder->CreateLoad(bitsType, pointer, "atomic.float.initial");
    initial->setAlignment(alignment);
    initial->setAtomic(llvm::AtomicOrdering::Monotonic);
    builder->CreateBr(retryBlock);

    builder->SetInsertPoint(retryBlock);
    auto* expectedBits = builder->CreatePHI(bitsType, 2, "atomic.float.expected");
    expectedBits->addIncoming(initial, preheader);
    llvm::Value* oldValue = builder->CreateBitCast(expectedBits, floatType,
        "atomic.float.old");
    llvm::Value* normalizedOldValue = flushSubnormal(oldValue, builder);
    value = flushSubnormal(value, builder);
    llvm::Value* nextValue = op == llvm::AtomicRMWInst::FAdd
        ? builder->CreateFAdd(normalizedOldValue, value, "atomic.float.sum")
        : builder->CreateFSub(normalizedOldValue, value, "atomic.float.difference");
    nextValue = canonicalizeNaN(nextValue, builder);
    llvm::Value* nextBits = builder->CreateBitCast(nextValue, bitsType,
        "atomic.float.next");

    auto* exchange = builder->CreateAtomicCmpXchg(pointer, expectedBits, nextBits,
        alignment, llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::AtomicOrdering::Monotonic);
    exchange->setWeak(false);
    llvm::Value* observedBits = builder->CreateExtractValue(exchange, 0,
        "atomic.float.observed");
    llvm::Value* succeeded = builder->CreateExtractValue(exchange, 1,
        "atomic.float.exchanged");
    expectedBits->addIncoming(observedBits, retryBlock);
    builder->CreateCondBr(succeeded, doneBlock, retryBlock);

    builder->SetInsertPoint(doneBlock);
    // A successful compare-exchange observed exactly expectedBits. Return that
    // old representation unchanged, even when it contains a NaN payload.
    return builder->CreateBitCast(expectedBits, floatType, "atomic.float.result");
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
    trapIfNullPointer(ptr, builder);

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

    const auto& layout = builder->GetInsertBlock()->getModule()->getDataLayout();
    llvm::Align align = layout.getABITypeAlign(valType);
    if (op == llvm::AtomicRMWInst::FAdd || op == llvm::AtomicRMWInst::FSub) {
        const llvm::Align compareExchangeAlignment(
            valType->getPrimitiveSizeInBits() / 8);
        if (align < compareExchangeAlignment) align = compareExchangeAlignment;
    }
    trapIfAccessRangeWraps(ptr, valType, builder);
    trapIfMisalignedPointer(ptr, align, builder);
    if (op == llvm::AtomicRMWInst::FAdd || op == llvm::AtomicRMWInst::FSub)
        return processCanonicalAtomicFloatRmw(op, ptr, val, align, builder);
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
    trapIfNullPointer(ptr, builder);

    const llvm::Align align = builder->GetInsertBlock()->getModule()->getDataLayout()
        .getABITypeAlign(loadType);
    trapIfAccessRangeWraps(ptr, loadType, builder);
    trapIfMisalignedPointer(ptr, align, builder);
    llvm::LoadInst* load = builder->CreateLoad(loadType, ptr);
    load->setAlignment(align);
    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
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
    trapIfNullPointer(ptr, builder);

    const llvm::Align align = builder->GetInsertBlock()->getModule()->getDataLayout()
        .getABITypeAlign(val->getType());
    trapIfAccessRangeWraps(ptr, val->getType(), builder);
    trapIfMisalignedPointer(ptr, align, builder);
    llvm::StoreInst* store = builder->CreateStore(val, ptr);
    store->setAlignment(align);
    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
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
    trapIfNullPointer(ptr, builder);

    desired = coerceValue(desired, expected->getType(), builder, "'#cas' desired-value operand");
    if (!desired) {
        return nullptr;
    }

    const auto& layout = builder->GetInsertBlock()->getModule()->getDataLayout();
    const llvm::Align align = layout.getABITypeAlign(expected->getType());
    trapIfAccessRangeWraps(ptr, expected->getType(), builder);
    trapIfMisalignedPointer(ptr, align, builder);
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
        if (isWasm(s) || s.architecture == Architecture::LLVMGeneric) {
            if (!command.values.empty()) psi::ErrorStream() << "'#nop' takes no operands\n";
            return NativeInstructionResult::Handled;
        }
        assembly = s.architecture == Architecture::SystemZ ? "bcr 0, 0" : "nop";
    }
    if (name == "hlt" || name == "cli" || name == "sti") {
        if (!x86) {
            psi::ErrorStream() << "'#" << name << "' requires x86\n";
            return NativeInstructionResult::Handled;
        }
        assembly = name;
        memoryBarrier = true;
    }
    if (name == "cpu_relax") {
        if (!command.values.empty()) {
            psi::ErrorStream() << "'#cpu_relax' takes no operands\n";
            return NativeInstructionResult::Handled;
        }
        if (x86) assembly = "pause";
        else if (isARM(s.architecture)) assembly = "yield";
        else return NativeInstructionResult::Handled; // Advisory hint; no-op on other targets.
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

static llvm::Value* processMemoryIntrinsicInstruction(State& s, const std::string& name,
    const CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 3) {
        psi::ErrorStream() << "'#" << name << "' requires 3 operands\n";
        return nullptr;
    }

    llvm::Value* destination = processValue(s, *command.values[0], builder);
    llvm::Value* second = processValue(s, *command.values[1], builder);
    llvm::Value* length = processValue(s, *command.values[2], builder);
    const bool copy = name == "memcpy";
    if (!destination || !second || !length || !destination->getType()->isPointerTy()
        || (copy && !second->getType()->isPointerTy())
        || !length->getType()->isIntegerTy()) {
        psi::ErrorStream() << "'#" << name << "' requires pointer, "
            << (copy ? "pointer" : "integer byte") << ", integer size operands\n";
        return nullptr;
    }

    auto* intptr = builder->getIntPtrTy(builder->GetInsertBlock()->getModule()->getDataLayout());
    const unsigned pointerWidth = intptr->getIntegerBitWidth();
    if (length->getType()->getIntegerBitWidth() > pointerWidth) {
        llvm::Type* sourceType = length->getType();
        llvm::Value* narrowed = builder->CreateTrunc(length, intptr, "memory.length.narrow");
        llvm::Value* restored = builder->CreateZExt(narrowed, sourceType, "memory.length.restored");
        llvm::Value* tooLarge = builder->CreateICmpNE(length, restored, "memory.length.too_large");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        auto* trapBlock = llvm::BasicBlock::Create(builder->getContext(), "memory.length.trap", function);
        auto* convertBlock = llvm::BasicBlock::Create(builder->getContext(), "memory.length.ok", function);
        builder->CreateCondBr(tooLarge, trapBlock, convertBlock);
        builder->SetInsertPoint(trapBlock);
        llvm::Function* trap = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(), llvm::Intrinsic::trap);
        builder->CreateCall(trap);
        builder->CreateUnreachable();
        builder->SetInsertPoint(convertBlock);
        length = narrowed;
    } else {
        length = builder->CreateZExt(length, intptr);
    }

    trapIfAddressRangeWraps(destination, length, builder);
    if (copy) trapIfAddressRangeWraps(second, length, builder);

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    auto* accessBlock = llvm::BasicBlock::Create(builder->getContext(), "memory.nonzero", function);
    auto* doneBlock = llvm::BasicBlock::Create(builder->getContext(), "memory.done", function);
    llvm::Value* zeroLength = builder->CreateICmpEQ(length, llvm::ConstantInt::get(intptr, 0));
    builder->CreateCondBr(zeroLength, doneBlock, accessBlock);
    builder->SetInsertPoint(accessBlock);
    trapIfNullPointer(destination, builder);
    if (copy) trapIfNullPointer(second, builder);

    // Ordinary PSI memory operations are sequentially consistent byte accesses.
    // This both defines races with other PSI accesses and permits unaligned
    // transfers on targets that only support naturally aligned atomics.
    llvm::Type* byteType = builder->getInt8Ty();
    auto* destinationBytes = builder->CreateBitCast(destination,
        llvm::PointerType::get(byteType,
            llvm::cast<llvm::PointerType>(destination->getType())->getAddressSpace()));
    llvm::Value* sourceBytes = nullptr;
    llvm::Value* copyBackward = llvm::ConstantInt::getFalse(builder->getContext());
    if (copy) {
        sourceBytes = builder->CreateBitCast(second,
            llvm::PointerType::get(byteType,
                llvm::cast<llvm::PointerType>(second->getType())->getAddressSpace()));
        const auto& layout = builder->GetInsertBlock()->getModule()->getDataLayout();
        llvm::Type* addressType = builder->getIntNTy(layout.getPointerSizeInBits(0));
        llvm::Value* dstAddress = builder->CreatePtrToInt(destination, addressType);
        llvm::Value* srcAddress = builder->CreatePtrToInt(second, addressType);
        llvm::Value* destinationFollowsSource = builder->CreateICmpUGT(dstAddress, srcAddress);
        llvm::Value* distance = builder->CreateSub(dstAddress, srcAddress);
        copyBackward = builder->CreateAnd(destinationFollowsSource,
            builder->CreateICmpULT(distance, length));
    } else {
        second = builder->CreateZExtOrTrunc(second, byteType);
    }

    llvm::BasicBlock* loopBlock = llvm::BasicBlock::Create(
        builder->getContext(), "memory.byte.loop", function);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(
        builder->getContext(), "memory.byte.body", function);
    llvm::BasicBlock* copyDoneBlock = llvm::BasicBlock::Create(
        builder->getContext(), "memory.byte.done", function);
    llvm::BasicBlock* preheader = builder->GetInsertBlock();
    builder->CreateBr(loopBlock);
    builder->SetInsertPoint(loopBlock);
    auto* index = builder->CreatePHI(intptr, 2, "memory.byte.index");
    index->addIncoming(llvm::ConstantInt::get(intptr, 0), preheader);
    builder->CreateCondBr(builder->CreateICmpULT(index, length), bodyBlock, copyDoneBlock);

    builder->SetInsertPoint(bodyBlock);
    llvm::Value* byteIndex = index;
    if (copy) {
        llvm::Value* reverseIndex = builder->CreateSub(
            builder->CreateSub(length, llvm::ConstantInt::get(intptr, 1)), index);
        byteIndex = builder->CreateSelect(copyBackward, reverseIndex, index);
    }
    llvm::Value* destinationByteAddress = builder->CreateGEP(byteType,
        destinationBytes, byteIndex);
    llvm::Value* byteValue = second;
    if (copy) {
        llvm::Value* sourceByteAddress = builder->CreateGEP(byteType,
            sourceBytes, byteIndex);
        auto* byteLoad = builder->CreateLoad(byteType, sourceByteAddress);
        byteLoad->setAlignment(llvm::Align(1));
        byteLoad->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
        byteValue = byteLoad;
    }
    auto* byteStore = builder->CreateStore(byteValue, destinationByteAddress);
    byteStore->setAlignment(llvm::Align(1));
    byteStore->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    llvm::Value* nextIndex = builder->CreateAdd(index,
        llvm::ConstantInt::get(intptr, 1));
    index->addIncoming(nextIndex, bodyBlock);
    builder->CreateBr(loopBlock);

    builder->SetInsertPoint(copyDoneBlock);
    builder->CreateBr(doneBlock);
    builder->SetInsertPoint(doneBlock);
    return nullptr;
}

static llvm::Value* processFloatToIntegerCast(State& s, const std::string& name,
    llvm::Value* operand, llvm::Type* resultType, llvm::IRBuilder<>* builder)
{
    if (!operand->getType()->isFloatingPointTy() || !resultType->isIntegerTy()) {
        psi::ErrorStream() << "'#" << name
                           << "' requires a floating-point source and integer result\n";
        return nullptr;
    }

    operand = flushSubnormal(operand, builder);

    // LLVM makes out-of-range FP-to-int conversions poison. PSI traps first.
    const bool signedResult = name == "fptosi";
    const unsigned resultBits = resultType->getIntegerBitWidth();
    const double magnitude = std::ldexp(1.0,
        signedResult ? resultBits - 1 : resultBits);
    const double lower = signedResult ? -magnitude : 0.0;
    llvm::Value* lowerBound = llvm::ConstantFP::get(operand->getType(), lower);
    llvm::Value* upperBound = llvm::ConstantFP::get(operand->getType(), magnitude);
    llvm::Value* inRange = builder->CreateAnd(
        builder->CreateFCmpOGE(operand, lowerBound),
        builder->CreateFCmpOLT(operand, upperBound));

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    auto* convertBlock = llvm::BasicBlock::Create(*s.context, "cast.in_range", function);
    auto* trapBlock = llvm::BasicBlock::Create(*s.context, "cast.out_of_range", function);
    auto* mergeBlock = llvm::BasicBlock::Create(*s.context, "cast.done", function);
    builder->CreateCondBr(inRange, convertBlock, trapBlock);

    builder->SetInsertPoint(trapBlock);
    llvm::Function* trap = llvm::Intrinsic::getDeclaration(
        builder->GetInsertBlock()->getModule(), llvm::Intrinsic::trap);
    builder->CreateCall(trap);
    builder->CreateUnreachable();

    builder->SetInsertPoint(convertBlock);
    llvm::Value* converted = signedResult
        ? builder->CreateFPToSI(operand, resultType)
        : builder->CreateFPToUI(operand, resultType);
    builder->CreateBr(mergeBlock);

    builder->SetInsertPoint(mergeBlock);
    auto* result = builder->CreatePHI(resultType, 1, "cast.result");
    result->addIncoming(converted, convertBlock);
    return result;
}

llvm::Value* processSpecialInstruction(State& s,
    const CommandNode& command,
    llvm::IRBuilder<>* builder,
    const TypeNode* declaredType)
{
    std::string name = command.instruction.name;
    std::replace(name.begin(), name.end(), '.', '_');

    const bool viewLength = name == "view_len" || name == "viewlen";
    const bool viewLoad = name == "view_load" || name == "viewload";
    const bool viewStore = name == "view_store" || name == "viewstore";
    const bool viewSlice = name == "view_slice" || name == "viewslice";
    auto convertViewInteger = [&](const ValueNode& source, llvm::Value* value,
                                  const std::string& context) {
        bool unsignedSource = source.kind == ValueKind::Bool
            || source.kind == ValueKind::SpecialRegister
            || isDeclaredUnsigned(s, source);
        if (!unsignedSource && source.kind == ValueKind::Register) {
            TypeNode sourceType;
            unsignedSource = resolveRegisterTypeNode(s, source.registerValue, sourceType)
                && sourceType.baseName == "bool";
        }
        return coerceValue(value, builder->getInt64Ty(), builder, context, unsignedSource);
    };
    if (viewLength || viewLoad || viewStore || viewSlice) {
        const std::size_t expected = viewLength ? 1u : viewLoad ? 2u : 3u;
        if (command.values.size() != expected) {
            psi::ErrorStream() << "'#" << name << "' requires " << expected << " operand(s)\n";
            return nullptr;
        }
        const ValueNode& viewNode = *command.values[0];
        TypeNode viewNodeType;
        if (viewNode.kind != ValueKind::Register
            || !resolveRegisterTypeNode(s, viewNode.registerValue, viewNodeType)
            || !viewNodeType.isView || !viewNode.registerValue.accessors.empty()) {
            psi::ErrorStream() << "'#" << name << "' requires a plain bounded-view register\n";
            return nullptr;
        }
        llvm::Value* view = processValue(s, viewNode, builder);
        if (!view) return nullptr;
        if (viewLength)
            return builder->CreateExtractValue(view, {1}, "view.length");

        if (viewSlice) {
            if (!declaredType || !declaredType->isView
                || viewNodeType.baseName != declaredType->baseName
                || viewNodeType.pointerLevel != declaredType->pointerLevel
                || !viewNodeType.isView) {
                psi::logError("'#viewslice' result must have the source view's element type");
                return nullptr;
            }
            llvm::Value* start = processValue(s, *command.values[1], builder);
            llvm::Value* count = processValue(s, *command.values[2], builder);
            if (!start || !count || !start->getType()->isIntegerTy()
                || !count->getType()->isIntegerTy()) {
                psi::logError("'#viewslice' start and count must be integers");
                return nullptr;
            }
            start = convertViewInteger(*command.values[1], start, "'#viewslice' start");
            count = convertViewInteger(*command.values[2], count, "'#viewslice' count");
            if (!start || !count) return nullptr;
            llvm::Value* length = builder->CreateExtractValue(view, {1}, "view.length");
            llvm::Value* startOutOfBounds = builder->CreateICmpUGT(start, length);
            llvm::Value* remaining = builder->CreateSub(length, start, "view.remaining");
            llvm::Value* countOutOfBounds = builder->CreateICmpUGT(count, remaining);
            llvm::Value* outOfBounds = builder->CreateOr(startOutOfBounds, countOutOfBounds,
                "view.slice.out_of_bounds");
            llvm::Function* function = builder->GetInsertBlock()->getParent();
            auto* trapBlock = llvm::BasicBlock::Create(*s.context, "view.slice.trap", function);
            auto* accessBlock = llvm::BasicBlock::Create(*s.context, "view.slice.ok", function);
            builder->CreateCondBr(outOfBounds, trapBlock, accessBlock);
            builder->SetInsertPoint(trapBlock);
            llvm::Function* trap = llvm::Intrinsic::getDeclaration(
                builder->GetInsertBlock()->getModule(), llvm::Intrinsic::trap);
            builder->CreateCall(trap);
            builder->CreateUnreachable();

            builder->SetInsertPoint(accessBlock);
            TypeNode elementTypeNode = viewNodeType;
            elementTypeNode.isView = false;
            llvm::Type* elementType = resolveType(s, elementTypeNode);
            if (!elementType) return nullptr;
            llvm::Value* data = builder->CreateExtractValue(view, {0}, "view.data");
            llvm::Value* slicedData = builder->CreateGEP(elementType, data, start, "view.slice.data");
            llvm::Type* resultType = resolveType(s, *declaredType);
            if (!resultType) return nullptr;
            llvm::Value* result = llvm::UndefValue::get(resultType);
            result = builder->CreateInsertValue(result, slicedData, {0});
            return builder->CreateInsertValue(result, count, {1}, "view.slice");
        }

        const std::size_t indexOperand = 1;
        llvm::Value* index = processValue(s, *command.values[indexOperand], builder);
        if (!index || !index->getType()->isIntegerTy()) {
            psi::ErrorStream() << "'#" << name << "' index must be an integer\n";
            return nullptr;
        }
        index = convertViewInteger(*command.values[indexOperand], index,
            "'#" + name + "' index");
        if (!index) return nullptr;
        llvm::Value* length = builder->CreateExtractValue(view, {1}, "view.length");
        llvm::Value* outOfBounds = builder->CreateICmpUGE(index, length, "view.out_of_bounds");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        auto* trapBlock = llvm::BasicBlock::Create(*s.context, "view.bounds.trap", function);
        auto* accessBlock = llvm::BasicBlock::Create(*s.context, "view.bounds.ok", function);
        builder->CreateCondBr(outOfBounds, trapBlock, accessBlock);

        builder->SetInsertPoint(trapBlock);
        llvm::Function* trap = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(), llvm::Intrinsic::trap);
        builder->CreateCall(trap);
        builder->CreateUnreachable();

        builder->SetInsertPoint(accessBlock);
        TypeNode elementTypeNode = viewNodeType;
        elementTypeNode.isView = false;
        llvm::Type* elementType = resolveType(s, elementTypeNode);
        if (!elementType) return nullptr;
        if (viewLoad && (!declaredType || resolveType(s, *declaredType) != elementType)) {
            psi::ErrorStream() << "'#viewload' result type must match the view element type\n";
            return nullptr;
        }
        llvm::Value* value = nullptr;
        if (viewStore) {
            value = processValue(s, *command.values[2], builder);
            if (!value || value->getType() != elementType) {
                psi::ErrorStream() << "'#viewstore' value type must match the view element type\n";
                return nullptr;
            }
        }
        llvm::Value* data = builder->CreateExtractValue(view, {0}, "view.data");
        trapIfNullPointer(data, builder);
        llvm::Value* address = builder->CreateGEP(elementType, data, index);
        // A view's length check protects the logical index. Keep the memory
        // access itself consistent with ordinary PSI accesses: null and
        // address-wrap trap, and byte alignment is sufficient.
        trapIfNullPointer(address, builder);
        trapIfAccessRangeWraps(address, elementType, builder);
        if (viewLoad) {
            return loadValueBytewiseAtomic(*builder, elementType, address);
        }
        // This helper emits byte-aligned stores and clears structure padding.
        storeValueWithZeroedPadding(*builder, value, address);
        return nullptr;
    }

    if (name == "sitofp" || name == "uitofp" || name == "fptosi" || name == "fptoui") {
        if (!declaredType || command.values.size() != 1) {
            psi::ErrorStream() << "'#" << name
                               << "' requires one operand and an explicitly typed result\n";
            return nullptr;
        }
        llvm::Value* operand = processValue(s, *command.values[0], builder);
        llvm::Type* resultType = resolveType(s, *declaredType);
        if (!operand || !resultType) return nullptr;

        if (name == "sitofp" || name == "uitofp") {
            if (!operand->getType()->isIntegerTy() || !resultType->isFloatingPointTy()) {
                psi::ErrorStream() << "'#" << name
                                   << "' requires an integer source and floating-point result\n";
                return nullptr;
            }
            return name == "sitofp"
                ? builder->CreateSIToFP(operand, resultType)
                : builder->CreateUIToFP(operand, resultType);
        }

        return processFloatToIntegerCast(s, name, operand, resultType, builder);
    }

    if (name == "trunc" || name == "sext" || name == "zext"
        || name == "fpext" || name == "fptrunc") {
        if (!declaredType || command.values.size() != 1) {
            psi::ErrorStream() << "'#" << name
                               << "' requires one operand and an explicitly typed result\n";
            return nullptr;
        }
        llvm::Value* operand = processValue(s, *command.values[0], builder);
        llvm::Type* resultType = resolveType(s, *declaredType);
        if (!operand || !resultType) return nullptr;

        llvm::Type* sourceType = operand->getType();
        if (name == "trunc" || name == "sext" || name == "zext") {
            if (!sourceType->isIntegerTy() || !resultType->isIntegerTy()) {
                psi::ErrorStream() << "'#" << name << "' requires integer types\n";
                return nullptr;
            }
            if (name == "trunc") return builder->CreateTrunc(operand, resultType);
            return name == "sext"
                ? builder->CreateSExt(operand, resultType)
                : builder->CreateZExt(operand, resultType);
        }
        if (!sourceType->isFloatingPointTy() || !resultType->isFloatingPointTy()) {
            psi::ErrorStream() << "'#" << name << "' requires floating-point types\n";
            return nullptr;
        }
        operand = flushSubnormal(operand, builder);
        return name == "fpext"
            ? canonicalizeNaN(builder->CreateFPExt(operand, resultType), builder)
            : canonicalizeNaN(builder->CreateFPTrunc(operand, resultType), builder);
    }

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

    if (name == "clz" || name == "ctz" || name == "popcnt"
        || name == "bswap" || name == "bitreverse") {
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

    if (name == "memcpy" || name == "memset")
        return processMemoryIntrinsicInstruction(s, name, command, builder);

    if (name == "inb" || name == "outb") {
        if (!x86) {
            psi::ErrorStream() << "'#" << name << "' requires x86\n";
            return nullptr;
        }
        const std::size_t expected = name == "inb" ? 1 : 2;
        if (command.values.size() != expected) {
            psi::ErrorStream() << "'#" << name << "' requires " << expected << " operand(s)\n";
            return nullptr;
        }
        auto* port = processValue(s, *command.values[0], builder);
        if (!port || !port->getType()->isIntegerTy()) {
            psi::ErrorStream() << "'#" << name << "' port must be an integer\n";
            return nullptr;
        }
        port = builder->CreateZExtOrTrunc(port, builder->getInt16Ty());
        if (name == "inb") {
            auto* type = llvm::FunctionType::get(builder->getInt8Ty(), {builder->getInt16Ty()}, false);
            return builder->CreateCall(llvm::InlineAsm::get(type, "inb $1, $0", "={al},{dx},~{memory}", true), {port});
        }
        auto* value = processValue(s, *command.values[1], builder);
        if (!value || !value->getType()->isIntegerTy()) {
            psi::ErrorStream() << "'#outb' value must be an integer\n";
            return nullptr;
        }
        value = builder->CreateZExtOrTrunc(value, builder->getInt8Ty());
        auto* type = llvm::FunctionType::get(builder->getVoidTy(), {builder->getInt8Ty(), builder->getInt16Ty()}, false);
        builder->CreateCall(llvm::InlineAsm::get(type, "outb $0, $1", "{al},{dx},~{memory}", true), {value, port});
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

    if (name == "cas" || name == "atomic_cas") {
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
        }
        trapIfNullPointer(pointer, builder);
        trapIfAccessRangeWraps(pointer, value->getType(), builder);
        storeValueWithZeroedPadding(*builder, value, pointer);
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
        if (hasInitializer) storeValueWithZeroedPadding(*builder, value, existing->second);
        return;
    }

    auto storage = s.localStorage.find(name);
    llvm::Value* slot = storage != s.localStorage.end() ? storage->second : nullptr;
    if (!slot) slot = builder->CreateAlloca(type, nullptr, name);
    const llvm::Align naturalAlignment = builder->GetInsertBlock()->getModule()
        ->getDataLayout().getABITypeAlign(type);
    const unsigned requestedAlignment = command.declaredType.alignment > 0
        ? static_cast<unsigned>(command.declaredType.alignment) : 0;
    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(slot))
        alloca->setAlignment(llvm::Align(std::max<std::uint64_t>(
            naturalAlignment.value(), requestedAlignment)));
    s.locals[name] = slot;
    s.localTypes[name] = command.declaredType;
    s.declaredLocalNames.push_back(name);
    if (hasInitializer) storeValueWithZeroedPadding(*builder, value, slot);
}

void processRegisterAssignment(State& s, const CommandNode& command, llvm::IRBuilder<>* builder)
{
    llvm::Value* value = computeCommandValue(s, command, builder, nullptr);
    if (!value) return;

    RegisterAddress access = resolveRegisterAddress(s, command.targetRegister, builder);
    if (!access.address) return;
    llvm::Type* targetType = resolveType(s, access.typeNode);
    if (!targetType) return;
        if (std::any_of(command.targetRegister.accessors.begin(),
                command.targetRegister.accessors.end(),
                [](const AccessorNode& accessor) { return accessor.kind == AccessorKind::Index; })) {
            trapIfNullPointer(access.address, builder);
            trapIfAccessRangeWraps(access.address, targetType, builder);
        }
    value = coerceValue(value, targetType, builder, "assignment to '" + command.targetRegister.name + "'",
        isUnsignedTypeName(access.typeNode.baseName));
    if (value) {
        storeValueWithZeroedPadding(*builder, value, access.address);
    }
}

llvm::Value* processBinaryInstruction(State& s, const std::string& op,
    const CommandNode& command, llvm::IRBuilder<>* builder)
{
    llvm::Value* lhs = !command.values.empty() ? processValue(s, *command.values[0], builder) : nullptr;
    llvm::Value* rhs = command.values.size() > 1 ? processValue(s, *command.values[1], builder) : nullptr;
        if (!lhs || !rhs) return nullptr;

    auto operandType = [&](const ValueNode& value, TypeNode& type) {
        if (value.kind == ValueKind::Register) {
            RegisterAddress address = resolveRegisterAddress(s, value.registerValue, builder);
            if (!address.address) return false;
            type = address.typeNode;
            return true;
        }
        if (value.kind == ValueKind::String) { type = {"i8", 1}; return true; }
        return false;
    };
    TypeNode leftType, rightType;
    const bool haveLeftType = operandType(*command.values[0], leftType);
    const bool haveRightType = operandType(*command.values[1], rightType);
    const bool leftPointer = haveLeftType && leftType.pointerLevel > 0;
    const bool rightPointer = haveRightType && rightType.pointerLevel > 0;
    if ((op == "add" && (leftPointer || rightPointer)) || (op == "sub" && leftPointer)) {
        const bool pointerOnLeft = leftPointer;
        const TypeNode pointerType = pointerOnLeft ? leftType : rightType;
        TypeNode elementTypeNode = pointerType;
        --elementTypeNode.pointerLevel;
        llvm::Type* elementType = resolveType(s, elementTypeNode);
        if (!elementType) return nullptr;
        const auto elementSize = builder->GetInsertBlock()->getModule()->getDataLayout().getTypeAllocSize(elementType);
        if (elementSize.isScalable() || elementSize.getFixedValue() == 0) {
            psi::logError("pointer arithmetic requires a nonzero, fixed object size");
            return nullptr;
        }
        llvm::Value* pointer = pointerOnLeft ? lhs : rhs;
        llvm::Value* offset = pointerOnLeft ? rhs : lhs;
        if (op == "sub") {
            llvm::Type* intptr = builder->getIntPtrTy(builder->GetInsertBlock()->getModule()->getDataLayout());
            llvm::Value* leftAddress = builder->CreatePtrToInt(lhs, intptr);
            llvm::Value* rightAddress = builder->CreatePtrToInt(rhs, intptr);
            llvm::Value* addressDelta = builder->CreateSub(leftAddress, rightAddress,
                "ptr.diff.bytes");

            // Interpret the wrapped address delta as signed, while keeping the
            // object size positive even when it exceeds the signed pointer
            // range. The extra bit also prevents sdiv's min/-1 poison case.
            const unsigned pointerBits = intptr->getIntegerBitWidth();
            llvm::Type* divisionType = builder->getIntNTy(pointerBits + 1);
            llvm::Value* signedDelta = builder->CreateSExt(addressDelta, divisionType,
                "ptr.diff.signed_bytes");
            llvm::Value* positiveElementSize = llvm::ConstantInt::get(
                divisionType, elementSize.getFixedValue());
            llvm::Value* elementDelta = builder->CreateSDiv(
                signedDelta, positiveElementSize, "ptr.diff.elements");
            return builder->CreateTrunc(elementDelta, intptr, "ptr.diff.result");
        }
        if (!offset->getType()->isIntegerTy()) {
            psi::logError("pointer arithmetic requires an integer offset");
            return nullptr;
        }
        const auto& layout = builder->GetInsertBlock()->getModule()->getDataLayout();
        const unsigned addressSpace = pointer->getType()->getPointerAddressSpace();
        const unsigned indexBits = layout.getIndexSizeInBits(addressSpace);
        llvm::Type* indexType = llvm::IntegerType::get(builder->getContext(), indexBits);
        const ValueNode& offsetNode = *command.values[pointerOnLeft ? 1 : 0];
        const bool unsignedOffset = isDeclaredUnsigned(s, offsetNode);
        const unsigned offsetBits = offset->getType()->getIntegerBitWidth();
        if (offsetBits < indexBits)
            offset = unsignedOffset ? builder->CreateZExt(offset, indexType) : builder->CreateSExt(offset, indexType);
        else if (offsetBits > indexBits)
            offset = builder->CreateTrunc(offset, indexType);
        return builder->CreateGEP(elementType, pointer, offset, "ptr.offset");
    }
    if ((leftPointer || rightPointer || command.values[0]->kind == ValueKind::Null
            || command.values[1]->kind == ValueKind::Null)
        && (op == "eq" || op == "neq" || op == "gt" || op == "lt" || op == "gte" || op == "lte")) {
        if (op == "eq") return builder->CreateICmpEQ(lhs, rhs);
        if (op == "neq") return builder->CreateICmpNE(lhs, rhs);
        if (op == "gt") return builder->CreateICmpUGT(lhs, rhs);
        if (op == "lt") return builder->CreateICmpULT(lhs, rhs);
        if (op == "gte") return builder->CreateICmpUGE(lhs, rhs);
        return builder->CreateICmpULE(lhs, rhs);
    }

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

    if (isFloat) {
        lhs = flushSubnormal(lhs, builder);
        rhs = flushSubnormal(rhs, builder);
    }

    if (op == "add") return isFloat
        ? canonicalizeNaN(builder->CreateFAdd(lhs, rhs), builder) : builder->CreateAdd(lhs, rhs);
    if (op == "sub") return isFloat
        ? canonicalizeNaN(builder->CreateFSub(lhs, rhs), builder) : builder->CreateSub(lhs, rhs);
    if (op == "mul") return isFloat
        ? canonicalizeNaN(builder->CreateFMul(lhs, rhs), builder) : builder->CreateMul(lhs, rhs);
    if (op == "div") {
        if (isFloat) return canonicalizeNaN(builder->CreateFDiv(lhs, rhs), builder);
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
        trapIfCondition(invalid, "integer.division", builder);
        return isUnsigned ? builder->CreateUDiv(lhs, rhs) : builder->CreateSDiv(lhs, rhs);
    }
    if (op == "mod") {
        if (isFloat) return canonicalizeNaN(builder->CreateFRem(lhs, rhs), builder);
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
        trapIfCondition(invalid, "integer.remainder", builder);
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
        trapIfCondition(tooLarge, "integer.shift", builder);
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
            return processCallInstruction(s, command, builder, declaredType);
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
            trapIfNullPointer(p, builder);
            trapIfAccessRangeWraps(p, type, builder);
            return loadValueBytewiseAtomic(*builder, type, p);
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
    psi::DiagnosticLocationScope diagnosticLocation(command.location.line,
        command.location.column);
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
    s.localStorage.clear();
    s.localTypes.clear();
    s.declaredLocalNames.clear();

    s.currentFunction = function;
    auto returnTypeIt = s.functionReturnTypes.find(diagnosticName);
    s.currentFunctionReturnType = (returnTypeIt != s.functionReturnTypes.end()) ? returnTypeIt->second : TypeNode { };

    llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(*s.context, "entry", function);
    llvm::IRBuilder<> builder(*s.context);
    builder.SetInsertPoint(entry_block);

    // Reserve all local storage in the entry block. A jump may enter after a
    // declaration, so stack storage must not depend on reaching that command.
    llvm::IRBuilder<> storageBuilder(entry_block, entry_block->begin());
    for (const CommandNode& command : body.commands) {
        if (!command.hasDeclaredType) continue;
        llvm::Type* localType = resolveType(s, command.declaredType);
        if (!localType) continue;
        auto* slot = storageBuilder.CreateAlloca(localType, nullptr, command.targetRegister.name);
        const auto naturalAlignment = function->getParent()->getDataLayout().getABITypeAlign(localType);
        const unsigned requestedAlignment = command.declaredType.alignment > 0
            ? static_cast<unsigned>(command.declaredType.alignment) : 0;
        slot->setAlignment(llvm::Align(std::max<std::uint64_t>(
            naturalAlignment.value(), requestedAlignment)));
        s.localStorage[command.targetRegister.name] = slot;
        // Every local has a defined initial value, even when control flow
        // jumps past its declaration. A declaration initializer overwrites it
        // when that command is reached.
        storeValueWithZeroedPadding(storageBuilder,
            llvm::Constant::getNullValue(localType), slot);
    }

    if (args) {
        size_t idx = 0;
        for (auto& arg : function->args()) {
            const ArgNode& argNode = (*args)[idx];
            llvm::Type* argType = arg.getType();
            auto* slot = builder.CreateAlloca(argType, nullptr, argNode.name);
            storeValueWithZeroedPadding(builder, &arg, slot);
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
            if (!command.isEmpty) {
                psi::DiagnosticLocationScope diagnosticLocation(command.location.line,
                    command.location.column);
                psi::logError("instruction after a terminator requires a label");
            }
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
