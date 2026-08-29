#include "codegen.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <unordered_map>

std::unordered_map<std::string, llvm::Type*> types;

std::unordered_map<std::string, llvm::FunctionType*> functions;
std::unordered_map<std::string, llvm::Function*> function_values;
std::unordered_map<std::string, std::vector<TypeNode>> function_param_declared_types;
std::unordered_map<std::string, TypeNode> function_return_declared_types;

std::unordered_map<std::string, llvm::GlobalVariable*> globals;
std::unordered_map<std::string, TypeNode> global_declared_types;

std::unordered_map<std::string, std::unordered_map<std::string, int>> struct_field_index;
std::unordered_map<std::string, std::vector<TypeNode>> struct_field_types;

std::unordered_map<std::string, llvm::Value*> values;
std::unordered_map<std::string, TypeNode> value_declared_types;
std::unordered_map<std::string, llvm::BasicBlock*> labels;
std::vector<std::string> used_values;

llvm::LLVMContext* current_context;
llvm::Function* current_function = nullptr;
TypeNode current_function_return_type;
Architecture current_architecture = Architecture::X86_64;
OperatingSystem current_os = OperatingSystem::Linux;

llvm::Type* resolveType(const TypeNode& type)
{
    auto it = types.find(type.baseName);
    if (it == types.end()) {
        psi::ErrorStream() << "unknown type '" << type.baseName << "'\n";
        return nullptr;
    }
    llvm::Type* result = it->second;
    for (int i = 0; i < type.pointerLevel; i++) {
        result = result->getPointerTo();
    }
    return result;
}

llvm::Constant* buildStringConstant(llvm::Module& module, const std::string& str)
{
    auto* strConstant = llvm::ConstantDataArray::getString(*current_context, str, true);
    auto* global = new llvm::GlobalVariable(
        module, strConstant->getType(), true,
        llvm::GlobalValue::PrivateLinkage, strConstant, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return global;
}

llvm::Constant* buildConstant(ValueNode* value, const TypeNode& declaredType, llvm::Module& module)
{
    llvm::Type* targetType = resolveType(declaredType);
    if (!targetType || !value) {
        return nullptr;
    }

    if (value->kind == ValueKind::Number) {
        if (targetType->isFloatingPointTy()) {
            double d = value->numberIsFloat ? value->numberAsFloat : (double)value->numberAsInt;
            return llvm::ConstantFP::get(targetType, d);
        }
        long long i = value->numberIsFloat ? (long long)value->numberAsFloat : value->numberAsInt;
        return llvm::ConstantInt::get(targetType, (uint64_t)i, true);
    }

    if (value->kind == ValueKind::String) {

        return buildStringConstant(module, value->stringValue);
    }

    if (value->kind == ValueKind::Bool) {
        if (!targetType->isIntegerTy()) {
            psi::ErrorStream() << "a bool literal needs an integer (e.g. bool/i8/i32) type\n";
            return nullptr;
        }
        return llvm::ConstantInt::get(targetType, value->boolValue ? 1 : 0);
    }

    if (value->kind == ValueKind::Null) {
        if (declaredType.pointerLevel < 1) {
            psi::ErrorStream() << "'null' needs a pointer type, e.g. " << declaredType.baseName << "*\n";
            return nullptr;
        }
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(*current_context, 0));
    }

    if (value->kind == ValueKind::Array) {
        if (declaredType.pointerLevel < 1) {
            psi::ErrorStream() << "array initializer for '" << declaredType.baseName
                               << "' needs a pointer type, e.g. " << declaredType.baseName << "*\n";
            return nullptr;
        }
        TypeNode elementType = declaredType;
        elementType.pointerLevel -= 1;
        llvm::Type* elemLlvmType = resolveType(elementType);
        if (!elemLlvmType) {
            return nullptr;
        }

        std::vector<llvm::Constant*> elements;
        for (auto* element : value->arrayValues) {
            llvm::Constant* c = buildConstant(element, elementType, module);
            if (!c) {
                return nullptr;
            }
            elements.push_back(c);
        }

        auto* arrayType = llvm::ArrayType::get(elemLlvmType, elements.size());
        auto* arrayConstant = llvm::ConstantArray::get(arrayType, elements);
        auto* global = new llvm::GlobalVariable(
            module, arrayType, true,
            llvm::GlobalValue::PrivateLinkage, arrayConstant, ".arr");

        return global;
    }

    psi::ErrorStream() << "unsupported initializer kind for a global/const value\n";
    return nullptr;
}

enum class RegisterWidth {
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
};

struct SpecialRegisterInfo {
    std::string constraint;
    RegisterWidth width;
};

std::unordered_map<std::string, SpecialRegisterInfo> special_registers;

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

void addRegister(const std::string& name, RegisterWidth width)
{
    special_registers[name] = SpecialRegisterInfo { "{" + name + "}", width };
}

static bool isAArch64(Architecture arch)
{
    return arch == Architecture::AArch64;
}

static bool isARM32(Architecture arch)
{
    return arch == Architecture::ARM;
}

static bool isARM(Architecture arch)
{
    return isARM32(arch) || isAArch64(arch);
}

void buildSpecialRegisterTable(Architecture arch)
{
    special_registers.clear();

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
            addRegister(name, RegisterWidth::I64);
        for (auto name : gpr32)
            addRegister(name, RegisterWidth::I32);
        for (auto name : gpr16)
            addRegister(name, RegisterWidth::I16);
        for (auto name : gpr8)
            addRegister(name, RegisterWidth::I8);

        for (int i = 0; i < 16; ++i)
            addRegister("xmm" + std::to_string(i), RegisterWidth::F32);
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
            addRegister(name, RegisterWidth::I32);
        for (auto name : gpr16)
            addRegister(name, RegisterWidth::I16);
        for (auto name : gpr8)
            addRegister(name, RegisterWidth::I8);

        for (int i = 0; i < 8; ++i)
            addRegister("xmm" + std::to_string(i), RegisterWidth::F32);
    } else if (isAArch64(arch)) {
        for (int i = 0; i < 31; ++i) {
            addRegister("x" + std::to_string(i), RegisterWidth::I64);
            addRegister("w" + std::to_string(i), RegisterWidth::I32);
        }

        for (int i = 0; i < 32; ++i) {
            addRegister("s" + std::to_string(i), RegisterWidth::F32);
            addRegister("d" + std::to_string(i), RegisterWidth::F64);
        }

        addRegister("sp", RegisterWidth::I64);

        addRegister("fp", RegisterWidth::I64);
        addRegister("lr", RegisterWidth::I64);
    } else if (isARM32(arch)) {
        for (int i = 0; i < 13; ++i)
            addRegister("r" + std::to_string(i), RegisterWidth::I32);

        addRegister("sp", RegisterWidth::I32);
        addRegister("lr", RegisterWidth::I32);

        addRegister("fp", RegisterWidth::I32);

        for (int i = 0; i < 32; ++i)
            addRegister("s" + std::to_string(i), RegisterWidth::F32);

        for (int i = 0; i < 16; ++i)
            addRegister("d" + std::to_string(i), RegisterWidth::F64);
    }
}

struct RegisterAddress {
    llvm::Value* address = nullptr;
    TypeNode typeNode;
};

RegisterAddress resolveRegisterAddress(const RegNode& reg, llvm::IRBuilder<>* builder)
{
    RegisterAddress result;

    llvm::Value* address = nullptr;
    TypeNode currentType;

    auto localIt = values.find(reg.name);
    if (localIt != values.end()) {
        address = localIt->second;
        currentType = value_declared_types[reg.name];
    } else {
        auto globIt = globals.find(reg.name);
        if (globIt != globals.end()) {
            address = globIt->second;
            currentType = global_declared_types[reg.name];
        } else {
            psi::ErrorStream() << "use of undeclared register '" << reg.name << "'\n";
            return result;
        }
    }

    for (const auto& accessor : reg.accessors) {
        if (accessor.kind == AccessorKind::Field) {
            if (currentType.pointerLevel != 0) {
                psi::ErrorStream() << "'.' field access on '" << reg.name
                                   << "' needs a struct value, not a pointer (use '[ ]' to index through a pointer first)\n";
                return { };
            }
            auto structIt = struct_field_index.find(currentType.baseName);
            if (structIt == struct_field_index.end()) {
                psi::ErrorStream() << "'" << currentType.baseName << "' is not a struct type\n";
                return { };
            }
            auto fieldIt = structIt->second.find(accessor.fieldName);
            if (fieldIt == structIt->second.end()) {
                psi::ErrorStream() << "struct '" << currentType.baseName
                                   << "' has no field named '" << accessor.fieldName << "'\n";
                return { };
            }

            llvm::Type* structLlvmType = resolveType(currentType);
            if (!structLlvmType) {
                return { };
            }
            address = builder->CreateStructGEP(structLlvmType, address, (unsigned)fieldIt->second);
            currentType = struct_field_types[currentType.baseName][fieldIt->second];
        } else {
            if (currentType.pointerLevel < 1) {
                psi::ErrorStream() << "'[ ]' index on '" << reg.name
                                   << "' needs a pointer value (use '.' to access a struct field instead)\n";
                return { };
            }

            llvm::Type* pointerLlvmType = resolveType(currentType);
            if (!pointerLlvmType) {
                return { };
            }
            llvm::Value* pointerValue = builder->CreateLoad(pointerLlvmType, address);

            TypeNode pointeeType = currentType;
            pointeeType.pointerLevel -= 1;
            llvm::Type* elementLlvmType = resolveType(pointeeType);
            if (!elementLlvmType) {
                return { };
            }

            address = builder->CreateGEP(elementLlvmType, pointerValue, builder->getInt32(accessor.index));
            currentType = pointeeType;
        }
    }

    result.address = address;
    result.typeNode = currentType;
    return result;
}

bool isUnsignedTypeName(const std::string& baseName)
{
    return baseName == "u8" || baseName == "u16" || baseName == "u32" || baseName == "u64";
}

bool resolveRegisterTypeNode(const RegNode& reg, TypeNode& outType)
{
    TypeNode currentType;

    auto localIt = value_declared_types.find(reg.name);
    if (localIt != value_declared_types.end()) {
        currentType = localIt->second;
    } else {
        auto globIt = global_declared_types.find(reg.name);
        if (globIt != global_declared_types.end()) {
            currentType = globIt->second;
        } else {
            return false;
        }
    }

    for (const auto& accessor : reg.accessors) {
        if (accessor.kind == AccessorKind::Field) {
            if (currentType.pointerLevel != 0) {
                return false;
            }
            auto structIt = struct_field_index.find(currentType.baseName);
            if (structIt == struct_field_index.end()) {
                return false;
            }
            auto fieldIt = structIt->second.find(accessor.fieldName);
            if (fieldIt == structIt->second.end()) {
                return false;
            }
            currentType = struct_field_types[currentType.baseName][fieldIt->second];
        } else {
            if (currentType.pointerLevel < 1) {
                return false;
            }
            currentType.pointerLevel -= 1;
        }
    }

    outType = currentType;
    return true;
}

bool isDeclaredUnsigned(const ValueNode& value)
{
    if (value.kind != ValueKind::Register) {
        return false;
    }
    TypeNode type;
    if (!resolveRegisterTypeNode(value.registerValue, type)) {
        return false;
    }
    return isUnsignedTypeName(type.baseName);
}

llvm::Value* processSpecialRegisterRead(const SpecialRegNode& reg, llvm::IRBuilder<>* builder)
{
    auto it = special_registers.find(reg.name);
    if (it == special_registers.end()) {
        psi::ErrorStream() << "'%" << reg.name << "' isn't a recognized register for this target\n";
        return nullptr;
    }

    llvm::Type* type = widthToType(it->second.width, *current_context);
    auto* asmType = llvm::FunctionType::get(type, false);
    auto* asmFn = llvm::InlineAsm::get(asmType, "", "=" + it->second.constraint, true);
    return builder->CreateCall(asmFn);
}

llvm::Value* processValue(ValueNode value, llvm::IRBuilder<>* builder)
{
    if (value.kind == ValueKind::Number) {
        if (value.numberIsFloat) {
            return llvm::ConstantFP::get(types["f32"], value.numberAsFloat);
        } else {
            return builder->getInt32(value.numberAsInt);
        }
    } else if (value.kind == ValueKind::String) {
        return builder->CreateGlobalStringPtr(value.stringValue);
    } else if (value.kind == ValueKind::Register) {
        RegisterAddress access = resolveRegisterAddress(value.registerValue, builder);
        if (!access.address) {
            return nullptr;
        }
        llvm::Type* llvmType = resolveType(access.typeNode);
        if (!llvmType) {
            return nullptr;
        }
        return builder->CreateLoad(llvmType, access.address);
    } else if (value.kind == ValueKind::SpecialRegister) {
        return processSpecialRegisterRead(value.specialRegisterValue, builder);
    } else if (value.kind == ValueKind::Bool) {
        return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*current_context), value.boolValue ? 1 : 0);
    } else if (value.kind == ValueKind::Null) {

        return llvm::ConstantPointerNull::get(llvm::PointerType::get(*current_context, 0));
    } else if (value.kind == ValueKind::Array) {
        psi::ErrorStream() << "array literals can only be used directly as a register's "
                              "initializer (e.g. 'i32* arr = [1, 2, 3];'), not as a general value\n";
        return nullptr;
    }
    return nullptr;
}

llvm::Value* coerceValue(llvm::Value* value, llvm::Type* targetType, llvm::IRBuilder<>* builder, const std::string& context, bool treatSourceAsUnsigned = false)
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

    if (fromType->isPointerTy() && targetType->isIntegerTy()) {
        return builder->CreatePtrToInt(value, targetType);
    }

    if (fromType->isIntegerTy() && targetType->isPointerTy()) {
        return builder->CreateIntToPtr(value, targetType);
    }

    psi::ErrorStream() << "type mismatch in " << context << ": can't use this value here";
    return nullptr;
}

llvm::Value* buildLocalArrayLiteral(ValueNode& arrayValue, const TypeNode& declaredType, llvm::IRBuilder<>* builder)
{
    if (declaredType.pointerLevel < 1) {
        psi::ErrorStream() << "array initializer for '" << declaredType.baseName
                           << "' needs a pointer type, e.g. " << declaredType.baseName << "*\n";
        return nullptr;
    }

    TypeNode elementTypeNode = declaredType;
    elementTypeNode.pointerLevel -= 1;
    llvm::Type* elemType = resolveType(elementTypeNode);
    if (!elemType) {
        return nullptr;
    }

    std::vector<llvm::Value*> elements;
    for (auto* elementNode : arrayValue.arrayValues) {
        llvm::Value* v = processValue(*elementNode, builder);
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

void predeclareLabels(const std::vector<CommandNode>& commands, llvm::Function* function)
{
    for (const auto& command : commands) {
        if (command.isEmpty || command.targetKind != TargetKind::None || !command.hasInstruction) {
            continue;
        }
        if (command.instruction.isSpecial || command.instruction.name != "label") {
            continue;
        }
        const std::string& labelName = command.values[0]->registerValue.name;
        if (!labels.count(labelName)) {
            labels[labelName] = llvm::BasicBlock::Create(*current_context, labelName, function);
        }
    }
}

llvm::Value* processCallInstruction(CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.empty() || command.values[0]->kind != ValueKind::Register) {
        psi::ErrorStream() << "'call' needs a function name as its first operand\n";
        return nullptr;
    }

    const std::string& calleeName = command.values[0]->registerValue.name;
    auto fnIt = function_values.find(calleeName);
    if (fnIt == function_values.end()) {
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

    auto paramTypesIt = function_param_declared_types.find(calleeName);

    std::vector<llvm::Value*> args;
    for (size_t i = 1; i < command.values.size(); i++) {
        llvm::Value* arg = processValue(*command.values[i], builder);
        if (!arg) {
            return nullptr;
        }
        llvm::Type* paramType = callee->getFunctionType()->getParamType(i - 1);
        bool paramIsUnsigned = paramTypesIt != function_param_declared_types.end()
            && isUnsignedTypeName(paramTypesIt->second[i - 1].baseName);
        arg = coerceValue(arg, paramType, builder, "argument " + std::to_string(i) + " to '" + calleeName + "'", paramIsUnsigned);
        if (!arg) {
            return nullptr;
        }
        args.push_back(arg);
    }

    return builder->CreateCall(callee, args);
}

llvm::Value* processRefInstruction(CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.empty() || command.values[0]->kind != ValueKind::Register) {
        psi::ErrorStream() << "'ref' needs a register operand\n";
        return nullptr;
    }
    RegisterAddress access = resolveRegisterAddress(command.values[0]->registerValue, builder);
    return access.address;
}

void processSpecialRegisterWrite(const SpecialRegNode& reg, llvm::Value* value, llvm::IRBuilder<>* builder)
{
    auto it = special_registers.find(reg.name);
    if (it == special_registers.end()) {
        psi::ErrorStream() << "'%" << reg.name << "' isn't a recognized register for this target\n";
        return;
    }

    llvm::Type* type = widthToType(it->second.width, *current_context);
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

    auto* asmType = llvm::FunctionType::get(llvm::Type::getVoidTy(*current_context), { type }, false);
    auto* asmFn = llvm::InlineAsm::get(asmType, "", it->second.constraint, true);
    builder->CreateCall(asmFn, { value });
}

llvm::Value* processSyscallInstruction(CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (current_os == OperatingSystem::FreeStanding) {
        psi::ErrorStream() << "'#syscall' needs a target OS - there's no syscall ABI "
                              "on a freestanding/bare-metal target\n";

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

    const bool wide = current_architecture == Architecture::X86_64 || isAArch64(current_architecture);

    llvm::Type* wordType = wide
        ? llvm::Type::getInt64Ty(*current_context)
        : llvm::Type::getInt32Ty(*current_context);

    std::vector<llvm::Value*> operands;
    std::vector<llvm::Type*> operandTypes;

    for (auto* v : command.values) {
        llvm::Value* val = processValue(*v, builder);
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

    auto* asmType = llvm::FunctionType::get(wordType, operandTypes, false);
    std::string constraints;
    std::string asmText;

    if (current_architecture == Architecture::X86_64) {

        static const char* regs[] = {
            "{rax}", "{rdi}", "{rsi}", "{rdx}", "{r10}", "{r8}", "{r9}"
        };

        constraints = "={rax}";
        for (size_t i = 0; i < operands.size(); ++i)
            constraints += "," + std::string(regs[i]);

        constraints += ",~{rcx},~{r11},~{memory}";
        asmText = "syscall";
    } else if (current_architecture == Architecture::X86) {

        static const char* regs[] = {
            "{eax}", "{ebx}", "{ecx}", "{edx}", "{esi}", "{edi}", "{ebp}"
        };

        constraints = "={eax}";
        for (size_t i = 0; i < operands.size(); ++i)
            constraints += "," + std::string(regs[i]);

        constraints += ",~{memory}";
        asmText = "int $$0x80";
    } else if (isAArch64(current_architecture)) {

        static const char* regs[] = {
            "{x8}", "{x0}", "{x1}", "{x2}", "{x3}", "{x4}", "{x5}"
        };

        constraints = "={x0}";
        for (size_t i = 0; i < operands.size(); ++i)
            constraints += "," + std::string(regs[i]);

        constraints += ",~{x8},~{memory}";
        asmText = "svc #0";
    } else if (isARM32(current_architecture)) {

        static const char* regs[] = {
            "{r7}", "{r0}", "{r1}", "{r2}", "{r3}", "{r4}", "{r5}"
        };

        constraints = "={r0}";
        for (size_t i = 0; i < operands.size(); ++i)
            constraints += "," + std::string(regs[i]);

        constraints += ",~{r7},~{memory}";
        asmText = "svc #0";
    } else {
        psi::ErrorStream() << "'#syscall' is not implemented for this architecture\n";
        return nullptr;
    }

    auto* asmFn = llvm::InlineAsm::get(
        asmType, asmText, constraints, true);

    return builder->CreateCall(asmFn, operands);
}

llvm::Value* processFloatArithmeticInstruction(
    const std::string& op,
    CommandNode& command,
    llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#" << op << "' needs exactly 2 operands\n";
        return nullptr;
    }

    llvm::Value* a = processValue(*command.values[0], builder);
    llvm::Value* b = processValue(*command.values[1], builder);
    if (!a || !b)
        return nullptr;

    llvm::Type* f32 = llvm::Type::getFloatTy(*current_context);
    llvm::Type* f64 = llvm::Type::getDoubleTy(*current_context);

    bool isDouble;
    if (a->getType() == f32 && b->getType() == f32) {
        isDouble = false;
    } else if (a->getType() == f64 && b->getType() == f64) {
        isDouble = true;
    } else {
        psi::ErrorStream() << "'#" << op
                           << "' needs two f32 operands or two f64 operands (not mixed)\n";
        return nullptr;
    }

    llvm::Type* operandType = isDouble ? f64 : f32;
    auto* asmType = llvm::FunctionType::get(operandType, { operandType, operandType }, false);

    std::string constraints;
    std::string asmText;

    if (current_architecture == Architecture::X86 || current_architecture == Architecture::X86_64) {

        static const std::unordered_map<std::string, std::string> singleMnemonics = {
            { "fadd", "addss" },
            { "fsub", "subss" },
            { "fmul", "mulss" },
            { "fdiv", "divss" },
        };
        static const std::unordered_map<std::string, std::string> doubleMnemonics = {
            { "fadd", "addsd" },
            { "fsub", "subsd" },
            { "fmul", "mulsd" },
            { "fdiv", "divsd" },
        };
        const auto& table = isDouble ? doubleMnemonics : singleMnemonics;
        auto it = table.find(op);
        if (it == table.end()) {
            psi::ErrorStream() << "unsupported floating-point instruction '#" << op << "'\n";
            return nullptr;
        }
        constraints = "=x,0,x";
        asmText = it->second + " $2, $0";

    } else if (isAArch64(current_architecture)) {

        static const std::unordered_map<std::string, std::string> mnemonics = {
            { "fadd", "fadd" },
            { "fsub", "fsub" },
            { "fmul", "fmul" },
            { "fdiv", "fdiv" },
        };
        auto it = mnemonics.find(op);
        if (it == mnemonics.end()) {
            psi::ErrorStream() << "unsupported floating-point instruction '#" << op << "'\n";
            return nullptr;
        }
        constraints = "=w,0,w";
        asmText = it->second + " $2, $0";

    } else if (isARM32(current_architecture)) {

        static const std::unordered_map<std::string, std::string> mnemonics = {
            { "fadd", "vadd" },
            { "fsub", "vsub" },
            { "fmul", "vmul" },
            { "fdiv", "vdiv" },
        };
        auto it = mnemonics.find(op);
        if (it == mnemonics.end()) {
            psi::ErrorStream() << "unsupported floating-point instruction '#" << op << "'\n";
            return nullptr;
        }
        constraints = isDouble ? "=w,0,w" : "=t,0,t";
        asmText = it->second + (isDouble ? ".f64" : ".f32") + " $2, $0";

    } else {
        psi::ErrorStream() << "'#" << op << "' is not implemented for this architecture\n";
        return nullptr;
    }

    auto* asmFn = llvm::InlineAsm::get(asmType, asmText, constraints, false);
    return builder->CreateCall(asmFn, { a, b });
}

static llvm::Value* processUnaryIntegerIntrinsic(
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
        return builder->CreateCall(fn, { value, llvm::ConstantInt::getFalse(*current_context) });
    }

    return builder->CreateCall(fn, { value });
}

static llvm::Value* processVectorArithmeticInstruction(
    const std::string& name,
    CommandNode& command,
    llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#" << name << "' needs exactly 2 operands\n";
        return nullptr;
    }

    llvm::Value* a = processValue(*command.values[0], builder);
    llvm::Value* b = processValue(*command.values[1], builder);
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

static llvm::Value* processVectorFmaInstruction(CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 3) {
        psi::ErrorStream() << "'#vfma' needs exactly 3 operands: a, b, c (computes a*b + c)\n";
        return nullptr;
    }

    llvm::Value* a = processValue(*command.values[0], builder);
    llvm::Value* b = processValue(*command.values[1], builder);
    llvm::Value* c = processValue(*command.values[2], builder);
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

static llvm::Value* processVectorSplatInstruction(
    CommandNode& command,
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

    llvm::Type* vecType = resolveType(*declaredType);
    if (!vecType) {
        return nullptr;
    }
    if (!vecType->isVectorTy()) {
        psi::ErrorStream() << "'#vsplat' needs a SIMD vector destination type (e.g. f32x4, i32x4)\n";
        return nullptr;
    }

    llvm::Value* scalar = processValue(*command.values[0], builder);
    if (!scalar) {
        return nullptr;
    }

    llvm::Type* laneType = llvm::cast<llvm::VectorType>(vecType)->getElementType();
    scalar = coerceValue(scalar, laneType, builder, "'#vsplat' operand", isDeclaredUnsigned(*command.values[0]));
    if (!scalar) {
        return nullptr;
    }

    unsigned numLanes = llvm::cast<llvm::FixedVectorType>(vecType)->getNumElements();
    return builder->CreateVectorSplat(numLanes, scalar);
}

static llvm::Value* processAtomicRMWInstruction(
    const std::string& name,
    llvm::AtomicRMWInst::BinOp op,
    CommandNode& command,
    llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#" << name << "' needs exactly 2 operands (pointer, value)\n";
        return nullptr;
    }

    llvm::Value* ptr = processValue(*command.values[0], builder);
    llvm::Value* val = processValue(*command.values[1], builder);
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

static llvm::Value* processAtomicLoadInstruction(
    CommandNode& command,
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

    llvm::Type* loadType = resolveType(*declaredType);
    if (!loadType) {
        return nullptr;
    }

    llvm::Value* ptr = processValue(*command.values[0], builder);
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

static llvm::Value* processAtomicStoreInstruction(CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 2) {
        psi::ErrorStream() << "'#atomicstore' needs exactly 2 operands (pointer, value)\n";
        return nullptr;
    }

    llvm::Value* ptr = processValue(*command.values[0], builder);
    llvm::Value* val = processValue(*command.values[1], builder);
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

static llvm::Value* processAtomicCasInstruction(CommandNode& command, llvm::IRBuilder<>* builder)
{
    if (command.values.size() != 3) {
        psi::ErrorStream() << "'#cas' needs exactly 3 operands (pointer, expected, desired)\n";
        return nullptr;
    }

    llvm::Value* ptr = processValue(*command.values[0], builder);
    llvm::Value* expected = processValue(*command.values[1], builder);
    llvm::Value* desired = processValue(*command.values[2], builder);
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

static llvm::Value* processSpecialInstruction(
    CommandNode& command,
    llvm::IRBuilder<>* builder,
    const TypeNode* declaredType)
{
    const std::string& name = command.instruction.name;

    if (name == "syscall")
        return processSyscallInstruction(command, builder);

    if (name == "clz" || name == "ctz" || name == "popcnt" || name == "bswap") {
        if (command.values.size() != 1) {
            psi::ErrorStream() << "'#" << name
                               << "' needs exactly 1 operand\n";
            return nullptr;
        }

        return processUnaryIntegerIntrinsic(
            name, processValue(*command.values[0], builder), builder);
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

        if (name == "pause" && current_architecture != Architecture::X86 && current_architecture != Architecture::X86_64) {
            psi::ErrorStream() << "'#pause' is only available on x86 targets\n";
            return nullptr;
        }

        if (name == "yield" && !isARM(current_architecture)) {
            psi::ErrorStream() << "'#yield' is only available on ARM targets\n";
            return nullptr;
        }

        auto* voidType = llvm::Type::getVoidTy(*current_context);
        auto* asmType = llvm::FunctionType::get(voidType, false);
        const char* asmText = name == "pause" ? "pause" : "yield";

        auto* asmFn = llvm::InlineAsm::get(
            asmType, asmText, "", true);

        builder->CreateCall(asmFn);
        return nullptr;
    }

    if (name == "rdtsc") {
        if (current_architecture != Architecture::X86 && current_architecture != Architecture::X86_64) {
            psi::ErrorStream() << "'#rdtsc' is only available on x86 targets\n";
            return nullptr;
        }

        llvm::Function* fn = llvm::Intrinsic::getDeclaration(
            builder->GetInsertBlock()->getModule(),
            llvm::Intrinsic::readcyclecounter);

        return builder->CreateCall(fn);
    }

    if (name == "fadd" || name == "fsub" || name == "fmul" || name == "fdiv") {
        return processFloatArithmeticInstruction(name, command, builder);
    }

    if (name == "vadd" || name == "vsub" || name == "vmul" || name == "vdiv"
        || name == "vdivi" || name == "vdivu" || name == "vand" || name == "vor"
        || name == "vxor" || name == "vmin" || name == "vmax") {
        return processVectorArithmeticInstruction(name, command, builder);
    }

    if (name == "vfma") {
        return processVectorFmaInstruction(command, builder);
    }

    if (name == "vsplat") {
        return processVectorSplatInstruction(command, builder, declaredType);
    }

    if (name == "atomicadd")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Add, command, builder);
    if (name == "atomicsub")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Sub, command, builder);
    if (name == "atomicand")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::And, command, builder);
    if (name == "atomicor")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Or, command, builder);
    if (name == "atomicxor")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Xor, command, builder);
    if (name == "atomicnand")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Nand, command, builder);
    if (name == "atomicxchg")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Xchg, command, builder);
    if (name == "atomicmax")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Max, command, builder);
    if (name == "atomicmin")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::Min, command, builder);
    if (name == "atomicumax")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::UMax, command, builder);
    if (name == "atomicumin")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::UMin, command, builder);
    if (name == "atomicfadd")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::FAdd, command, builder);
    if (name == "atomicfsub")
        return processAtomicRMWInstruction(name, llvm::AtomicRMWInst::FSub, command, builder);

    if (name == "atomicload") {
        return processAtomicLoadInstruction(command, builder, declaredType);
    }
    if (name == "atomicstore") {
        return processAtomicStoreInstruction(command, builder);
    }
    if (name == "cas") {
        return processAtomicCasInstruction(command, builder);
    }

    psi::ErrorStream() << "unsupported special instruction '#"
                       << name << "'\n";
    return nullptr;
}

llvm::Value* computeCommandValue(CommandNode& command, llvm::IRBuilder<>* builder, const TypeNode* declaredType)
{
    if (command.hasInstruction) {
        if (command.instruction.isSpecial) {
            return processSpecialInstruction(command, builder, declaredType);
        }

        const std::string& op = command.instruction.name;

        if (op == "call") {
            return processCallInstruction(command, builder);
        }
        if (op == "ref") {
            return processRefInstruction(command, builder);
        }
        if (op == "load") {
            auto* p = processValue(*command.values[0], builder);
            return builder->CreateLoad(resolveType(*declaredType), p);
        }
        if (op == "not") {
            auto* v = processValue(*command.values[0], builder);
            return v ? builder->CreateNot(v) : nullptr;
        }
        if (op == "lnot") {
            auto* v = processValue(*command.values[0], builder);
            return v ? builder->CreateICmpEQ(v, llvm::Constant::getNullValue(v->getType())) : nullptr;
        }

        llvm::Value* lhs = (!command.values.empty()) ? processValue(*command.values[0], builder) : nullptr;
        llvm::Value* rhs = (command.values.size() > 1) ? processValue(*command.values[1], builder) : nullptr;
        if (!lhs || !rhs) {
            return nullptr;
        }

        bool isUnsigned = isDeclaredUnsigned(*command.values[0]);
        rhs = coerceValue(rhs, lhs->getType(), builder, "'" + op + "'", isUnsigned);
        if (!rhs) {
            return nullptr;
        }

        bool isFloat = lhs->getType()->isFloatingPointTy();

        if (op == "add")
            return isFloat ? builder->CreateFAdd(lhs, rhs) : builder->CreateAdd(lhs, rhs);
        if (op == "sub")
            return isFloat ? builder->CreateFSub(lhs, rhs) : builder->CreateSub(lhs, rhs);
        if (op == "mul")
            return isFloat ? builder->CreateFMul(lhs, rhs) : builder->CreateMul(lhs, rhs);
        if (op == "div") {
            if (isFloat)
                return builder->CreateFDiv(lhs, rhs);
            return isUnsigned ? builder->CreateUDiv(lhs, rhs) : builder->CreateSDiv(lhs, rhs);
        }
        if (op == "mod") {
            if (isFloat)
                return builder->CreateFRem(lhs, rhs);
            return isUnsigned ? builder->CreateURem(lhs, rhs) : builder->CreateSRem(lhs, rhs);
        }
        if (op == "eq")
            return isFloat ? builder->CreateFCmpOEQ(lhs, rhs) : builder->CreateICmpEQ(lhs, rhs);
        if (op == "neq")
            return isFloat ? builder->CreateFCmpONE(lhs, rhs) : builder->CreateICmpNE(lhs, rhs);
        if (op == "gt") {
            if (isFloat)
                return builder->CreateFCmpOGT(lhs, rhs);
            return isUnsigned ? builder->CreateICmpUGT(lhs, rhs) : builder->CreateICmpSGT(lhs, rhs);
        }
        if (op == "lt") {
            if (isFloat)
                return builder->CreateFCmpOLT(lhs, rhs);
            return isUnsigned ? builder->CreateICmpULT(lhs, rhs) : builder->CreateICmpSLT(lhs, rhs);
        }
        if (op == "gte") {
            if (isFloat)
                return builder->CreateFCmpOGE(lhs, rhs);
            return isUnsigned ? builder->CreateICmpUGE(lhs, rhs) : builder->CreateICmpSGE(lhs, rhs);
        }
        if (op == "lte") {
            if (isFloat)
                return builder->CreateFCmpOLE(lhs, rhs);
            return isUnsigned ? builder->CreateICmpULE(lhs, rhs) : builder->CreateICmpSLE(lhs, rhs);
        }
        if (op == "and")
            return builder->CreateAnd(lhs, rhs);
        if (op == "or")
            return builder->CreateOr(lhs, rhs);
        if (op == "xor")
            return builder->CreateXor(lhs, rhs);
        if (op == "lsh")
            return builder->CreateShl(lhs, rhs);
        if (op == "rsh") {

            return isUnsigned ? builder->CreateLShr(lhs, rhs) : builder->CreateAShr(lhs, rhs);
        }
        if (op == "land") {
            auto* a = builder->CreateICmpNE(lhs, llvm::Constant::getNullValue(lhs->getType()));
            auto* b = builder->CreateICmpNE(rhs, llvm::Constant::getNullValue(rhs->getType()));
            return builder->CreateAnd(a, b);
        }
        if (op == "lor") {
            auto* a = builder->CreateICmpNE(lhs, llvm::Constant::getNullValue(lhs->getType()));
            auto* b = builder->CreateICmpNE(rhs, llvm::Constant::getNullValue(rhs->getType()));
            return builder->CreateOr(a, b);
        }

        psi::ErrorStream() << "unsupported instruction '" << op << "'\n";
        return nullptr;
    }

    if (declaredType && !command.values.empty() && command.values[0]->kind == ValueKind::Array) {
        return buildLocalArrayLiteral(*command.values[0], *declaredType, builder);
    }

    return processValue(*command.values[0], builder);
}

void processCommand(CommandNode command, llvm::IRBuilder<>* builder)
{
    if (command.isEmpty) {
        return;
    }

    if (command.targetKind == TargetKind::SpecialRegister) {
        llvm::Value* value = computeCommandValue(command, builder, nullptr);
        if (!value) {
            return;
        }
        processSpecialRegisterWrite(command.targetSpecialRegister, value, builder);
        return;
    }

    if (command.targetKind == TargetKind::None) {

        if (command.hasInstruction) {
            if (command.instruction.isSpecial) {
                processSpecialInstruction(command, builder, nullptr);
            } else {
                if (command.instruction.name == "ret") {
                    if (command.values.empty()) {
                        builder->CreateRetVoid();
                    } else {
                        llvm::Value* retValue = processValue(*command.values[0], builder);
                        if (!retValue) {
                            return;
                        }
                        retValue = coerceValue(retValue, current_function->getReturnType(), builder, "'ret'",
                            isUnsignedTypeName(current_function_return_type.baseName));
                        if (!retValue) {
                            return;
                        }
                        builder->CreateRet(retValue);
                    }
                } else if (command.instruction.name == "store") {
                    builder->CreateStore(processValue(*command.values[1], builder), processValue(*command.values[0], builder));
                } else if (command.instruction.name == "jmp") {
                    const std::string& target = command.values[0]->registerValue.name;
                    auto it = labels.find(target);
                    if (it == labels.end()) {
                        psi::ErrorStream() << "jmp to undefined label '" << target << "'\n";
                        return;
                    }
                    builder->CreateBr(it->second);
                } else if (command.instruction.name == "cjmp") {
                    const std::string& target = command.values[1]->registerValue.name;
                    auto it = labels.find(target);
                    if (it == labels.end()) {
                        psi::ErrorStream() << "cjmp to undefined label '" << target << "'\n";
                        return;
                    }
                    llvm::Value* condition = processValue(*command.values[0], builder);
                    if (!condition) {
                        return;
                    }
                    if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1)) {

                        condition = builder->CreateICmpNE(condition, llvm::Constant::getNullValue(condition->getType()));
                    } else if (!condition->getType()->isIntegerTy(1)) {
                        psi::ErrorStream() << "'cjmp' needs an integer condition";
                        return;
                    }
                    auto else_block = llvm::BasicBlock::Create(*current_context, "", current_function);
                    builder->CreateCondBr(condition, it->second, else_block);
                    builder->SetInsertPoint(else_block);
                } else if (command.instruction.name == "label") {
                    const std::string& labelName = command.values[0]->registerValue.name;

                    llvm::BasicBlock* block = labels.count(labelName)
                        ? labels[labelName]
                        : llvm::BasicBlock::Create(*current_context, labelName, current_function);
                    labels[labelName] = block;

                    if (!builder->GetInsertBlock()->getTerminator()) {
                        builder->CreateBr(block);
                    }
                    builder->SetInsertPoint(block);
                } else if (command.instruction.name == "call") {

                    processCallInstruction(command, builder);
                }
            }
        }
    } else if (command.targetKind == TargetKind::Register) {

        if (command.hasDeclaredType) {

            llvm::Type* reg_type = resolveType(command.declaredType);
            if (!reg_type) {
                return;
            }

            bool hasInitializer = command.hasInstruction || !command.values.empty();

            llvm::Value* value = nullptr;
            if (hasInitializer) {
                value = computeCommandValue(command, builder, &command.declaredType);
                if (!value) {
                    return;
                }
                value = coerceValue(value, reg_type, builder, "declaration of '" + command.targetRegister.name + "'",
                    isUnsignedTypeName(command.declaredType.baseName));
                if (!value) {
                    return;
                }
            }

            const std::string& reg_name = command.targetRegister.name;

            auto existing = values.find(reg_name);
            if (existing != values.end()) {

                if (hasInitializer) {
                    builder->CreateStore(value, existing->second);
                }
                return;
            }

            auto reg = builder->CreateAlloca(reg_type, nullptr, reg_name);
            if (command.declaredType.alignment > 0) {
                reg->setAlignment(llvm::Align(command.declaredType.alignment));
            }
            values[reg_name] = reg;
            value_declared_types[reg_name] = command.declaredType;
            used_values.push_back(reg_name);

            if (hasInitializer) {
                builder->CreateStore(value, reg);
            }
        } else {

            llvm::Value* value = computeCommandValue(command, builder, nullptr);
            if (!value) {
                return;
            }

            RegisterAddress access = resolveRegisterAddress(command.targetRegister, builder);
            if (!access.address) {
                return;
            }
            llvm::Type* targetType = resolveType(access.typeNode);
            if (!targetType) {
                return;
            }
            value = coerceValue(value, targetType, builder, "assignment to '" + command.targetRegister.name + "'",
                isUnsignedTypeName(access.typeNode.baseName));
            if (!value) {
                return;
            }
            builder->CreateStore(value, access.address);
        }
    }
}

void generateFunctionBody(const BlockNode& body, llvm::Function* function,
    const std::vector<ArgNode>* args, const std::string& diagnosticName)
{
    labels.clear();
    values.clear();
    value_declared_types.clear();
    used_values.clear();

    current_function = function;
    auto returnTypeIt = function_return_declared_types.find(diagnosticName);
    current_function_return_type = (returnTypeIt != function_return_declared_types.end()) ? returnTypeIt->second : TypeNode { };

    llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(*current_context, "entry", function);
    llvm::IRBuilder<> builder(*current_context);
    builder.SetInsertPoint(entry_block);

    if (args) {
        size_t idx = 0;
        for (auto& arg : function->args()) {
            const ArgNode& argNode = (*args)[idx];
            llvm::Type* argType = arg.getType();
            auto* slot = builder.CreateAlloca(argType, nullptr, argNode.name);
            builder.CreateStore(&arg, slot);
            values[argNode.name] = slot;
            value_declared_types[argNode.name] = argNode.type;
            idx++;
        }
    }

    predeclareLabels(body.commands, function);

    for (auto command : body.commands) {
        processCommand(command, &builder);
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

std::string defaultTriple(Architecture arch, OperatingSystem os)
{
    switch (os) {
    case OperatingSystem::Linux:
        switch (arch) {
        case Architecture::X86_64:
            return "x86_64-unknown-linux-gnu";
        case Architecture::X86:
            return "i386-unknown-linux-gnu";
        case Architecture::AArch64:
            return "aarch64-unknown-linux-gnu";
        case Architecture::ARM:
            return "armv7-unknown-linux-gnueabihf";
        }
        break;
    case OperatingSystem::Darwin:
        switch (arch) {
        case Architecture::X86_64:
            return "x86_64-apple-macosx";
        case Architecture::X86:
            return "i386-apple-macosx";
        case Architecture::AArch64:
            return "arm64-apple-macosx";
        case Architecture::ARM:
            return "armv7-apple-ios";
        }
        break;
    case OperatingSystem::Windows:
        switch (arch) {
        case Architecture::X86_64:
            return "x86_64-pc-windows-msvc";
        case Architecture::X86:
            return "i686-pc-windows-msvc";
        case Architecture::AArch64:
            return "aarch64-pc-windows-msvc";
        case Architecture::ARM:
            return "thumbv7-pc-windows-msvc";
        }
        break;
    case OperatingSystem::FreeStanding:
        switch (arch) {
        case Architecture::X86_64:
            return "x86_64-unknown-none";
        case Architecture::X86:
            return "i386-unknown-none";
        case Architecture::AArch64:
            return "aarch64-unknown-none";
        case Architecture::ARM:
            return "armv7-unknown-none-eabihf";
        }
        break;
    }
    return "x86_64-unknown-linux-gnu";
}

std::string compileProgram(std::string name, ProgramNode program, Architecture arch, OperatingSystem os)
{
    psi::resetErrors();

    llvm::LLVMContext context;
    current_context = &context;
    current_architecture = arch;
    current_os = os;
    buildSpecialRegisterTable(arch);

    llvm::Module module(name, context);
    module.setTargetTriple(defaultTriple(arch, os));

    types.clear();
    types["void"] = llvm::Type::getVoidTy(context);
    types["bool"] = llvm::Type::getInt1Ty(context);
    types["i8"] = llvm::Type::getInt8Ty(context);
    types["u8"] = llvm::Type::getInt8Ty(context);
    types["i16"] = llvm::Type::getInt16Ty(context);
    types["u16"] = llvm::Type::getInt16Ty(context);
    types["i32"] = llvm::Type::getInt32Ty(context);
    types["u32"] = llvm::Type::getInt32Ty(context);
    types["i64"] = llvm::Type::getInt64Ty(context);
    types["u64"] = llvm::Type::getInt64Ty(context);
    types["f32"] = llvm::Type::getFloatTy(context);
    types["f64"] = llvm::Type::getDoubleTy(context);

    types["i8x16"] = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(context), 16);
    types["i16x8"] = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(context), 8);
    types["i32x4"] = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(context), 4);
    types["i64x2"] = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(context), 2);
    types["f32x4"] = llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 4);
    types["f64x2"] = llvm::FixedVectorType::get(llvm::Type::getDoubleTy(context), 2);

    types["i32x8"] = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(context), 8);
    types["i64x4"] = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(context), 4);
    types["f32x8"] = llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 8);
    types["f64x4"] = llvm::FixedVectorType::get(llvm::Type::getDoubleTy(context), 4);

    functions.clear();
    function_values.clear();
    function_param_declared_types.clear();
    function_return_declared_types.clear();
    globals.clear();
    global_declared_types.clear();
    struct_field_index.clear();
    struct_field_types.clear();

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Struct) {
            const std::string& structName = dec.structDecl.type.baseName;
            if (!types.count(structName)) {
                types[structName] = llvm::StructType::create(context, structName);
            }
        }
    }

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Struct) {
            const std::string& structName = dec.structDecl.type.baseName;
            auto* structType = llvm::cast<llvm::StructType>(types[structName]);
            if (!structType->isOpaque()) {
                continue;
            }

            std::vector<llvm::Type*> fieldLlvmTypes;
            std::vector<TypeNode> fieldTypeNodes;
            std::unordered_map<std::string, int> fieldIndex;

            int idx = 0;
            for (auto& field : dec.structDecl.fields) {
                llvm::Type* fieldType = resolveType(field.type);
                fieldLlvmTypes.push_back(fieldType ? fieldType : llvm::Type::getInt8Ty(context));
                fieldTypeNodes.push_back(field.type);
                fieldIndex[field.name] = idx;
                idx++;
            }

            structType->setBody(fieldLlvmTypes);
            struct_field_types[structName] = fieldTypeNodes;
            struct_field_index[structName] = fieldIndex;
        }
    }

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Func) {
            if (function_values.count(dec.funcDecl.name)) {

                continue;
            }

            std::vector<llvm::Type*> argTypes;
            for (auto& arg : dec.funcDecl.args) {
                llvm::Type* argType = resolveType(arg.type);
                argTypes.push_back(argType ? argType : llvm::Type::getInt32Ty(context));
            }
            llvm::Type* retType = resolveType(dec.funcDecl.returnType);
            if (!retType) {
                retType = llvm::Type::getVoidTy(context);
            }

            auto* fnType = llvm::FunctionType::get(retType, argTypes, false);
            functions[dec.funcDecl.name] = fnType;

            std::vector<TypeNode> paramTypes;
            for (auto& arg : dec.funcDecl.args) {
                paramTypes.push_back(arg.type);
            }
            function_param_declared_types[dec.funcDecl.name] = paramTypes;
            function_return_declared_types[dec.funcDecl.name] = dec.funcDecl.returnType;

            auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, dec.funcDecl.name, module);
            size_t idx = 0;
            for (auto& arg : fn->args()) {
                arg.setName(dec.funcDecl.args[idx++].name);
            }
            function_values[dec.funcDecl.name] = fn;
        } else if (dec.kind == DeclKind::Glob || dec.kind == DeclKind::Const) {
            bool isConst = dec.kind == DeclKind::Const;
            const TypeNode& type = isConst ? dec.constDecl.type : dec.globDecl.type;
            const std::string& gname = isConst ? dec.constDecl.name : dec.globDecl.name;
            ValueNode* val = isConst ? dec.constDecl.value : dec.globDecl.value;

            llvm::Type* llvmType = resolveType(type);
            if (!llvmType) {
                continue;
            }
            llvm::Constant* init = buildConstant(val, type, module);
            if (!init) {
                psi::ErrorStream() << "could not build initializer for '" << gname << "'\n";
                continue;
            }

            auto* global = new llvm::GlobalVariable(
                module, llvmType, isConst, llvm::GlobalValue::ExternalLinkage, init, gname);
            globals[gname] = global;
            global_declared_types[gname] = type;
        }
    }

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Entry) {
            const std::string& entryName = dec.entryDecl.name;

            if (function_values.count(entryName)) {
                psi::ErrorStream() << "'" << entryName
                                   << "' is already declared as a func - entry needs its own name";
                continue;
            }

            llvm::FunctionType* entry_function_type = llvm::FunctionType::get(types["void"], { }, false);
            llvm::Function* entry_function = llvm::Function::Create(
                entry_function_type, llvm::Function::ExternalLinkage, entryName, module);

            generateFunctionBody(dec.entryDecl.body, entry_function, nullptr, entryName);
        } else if (dec.kind == DeclKind::Func) {
            llvm::Function* fn = function_values[dec.funcDecl.name];
            if (dec.funcDecl.hasBody) {
                generateFunctionBody(dec.funcDecl.body, fn, &dec.funcDecl.args, dec.funcDecl.name);
            }
        }
    }

    std::string moduleErrStr;
    llvm::raw_string_ostream moduleErrStream(moduleErrStr);
    if (llvm::verifyModule(module, &moduleErrStream)) {
        moduleErrStream.flush();
        psi::ErrorStream() << "module verification failed:\n"
                           << moduleErrStr << "\n";
    }

    std::string output;
    llvm::raw_string_ostream raw(output);
    raw << module;

    return output;
}

std::unique_ptr<llvm::TargetMachine> buildTargetMachine(const std::string& triple, std::string& errorMessage)
{
    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupError);
    if (!target) {
        errorMessage = "failed to look up target '" + triple + "': " + lookupError;
        return nullptr;
    }

    llvm::TargetOptions options;
    auto relocModel = llvm::Reloc::PIC_;
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(triple, "generic", "", options, relocModel));

    if (!targetMachine) {
        errorMessage = "failed to create a target machine for '" + triple + "'";
    }
    return targetMachine;
}

void runOptimizationPipeline(llvm::Module& module, llvm::TargetMachine* targetMachine, OptLevel level)
{
    if (level == OptLevel::O0) {
        return;
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(targetMachine);

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::OptimizationLevel llvmLevel;
    switch (level) {
    case OptLevel::O1:
        llvmLevel = llvm::OptimizationLevel::O1;
        break;
    case OptLevel::O2:
        llvmLevel = llvm::OptimizationLevel::O2;
        break;
    case OptLevel::O3:
        llvmLevel = llvm::OptimizationLevel::O3;
        break;
    case OptLevel::Os:
        llvmLevel = llvm::OptimizationLevel::Os;
        break;
    case OptLevel::Oz:
        llvmLevel = llvm::OptimizationLevel::Oz;
        break;
    default:
        return;
    }

    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvmLevel);
    MPM.run(module, MAM);
}

std::string optimizeIR(const std::string& irCode, OptLevel level, std::string& errorMessage)
{
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;

    std::unique_ptr<llvm::MemoryBuffer> irBuffer = llvm::MemoryBuffer::getMemBuffer(irCode, "module");
    std::unique_ptr<llvm::Module> module = llvm::parseIR(irBuffer->getMemBufferRef(), diagnostic, context);

    if (!module) {
        std::string diagText;
        llvm::raw_string_ostream diagStream(diagText);
        diagnostic.print("codegen", diagStream);
        diagStream.flush();
        errorMessage = "failed to parse IR: " + diagText;
        return "";
    }

    if (level != OptLevel::O0) {
        std::string triple = module->getTargetTriple();
        if (triple.empty()) {
            triple = llvm::sys::getDefaultTargetTriple();
        }

        std::unique_ptr<llvm::TargetMachine> targetMachine = buildTargetMachine(triple, errorMessage);
        if (!targetMachine) {
            return "";
        }

        module->setTargetTriple(triple);
        module->setDataLayout(targetMachine->createDataLayout());

        runOptimizationPipeline(*module, targetMachine.get(), level);
    }

    std::string output;
    llvm::raw_string_ostream raw(output);
    raw << *module;
    return output;
}

bool compileToObjectFile(
    const std::string& irCode,
    const std::string& targetTriple,
    const std::string& outputPath,
    OptLevel level,
    std::string& errorMessage)
{

    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;

    std::unique_ptr<llvm::MemoryBuffer> irBuffer = llvm::MemoryBuffer::getMemBuffer(irCode, "module");
    std::unique_ptr<llvm::Module> module = llvm::parseIR(irBuffer->getMemBufferRef(), diagnostic, context);

    if (!module) {
        std::string diagText;
        llvm::raw_string_ostream diagStream(diagText);
        diagnostic.print("codegen", diagStream);
        diagStream.flush();
        errorMessage = "failed to parse IR: " + diagText;
        return false;
    }

    std::string triple = targetTriple;
    if (triple.empty()) {
        triple = module->getTargetTriple();
    }
    if (triple.empty()) {
        triple = llvm::sys::getDefaultTargetTriple();
    }

    std::unique_ptr<llvm::TargetMachine> targetMachine = buildTargetMachine(triple, errorMessage);
    if (!targetMachine) {
        return false;
    }

    module->setTargetTriple(triple);
    module->setDataLayout(targetMachine->createDataLayout());

    runOptimizationPipeline(*module, targetMachine.get(), level);

    std::error_code fileError;
    llvm::raw_fd_ostream outputStream(outputPath, fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        errorMessage = "failed to open output file '" + outputPath + "': " + fileError.message();
        return false;
    }

    llvm::legacy::PassManager passManager;
    if (targetMachine->addPassesToEmitFile(passManager, outputStream, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        errorMessage = "target '" + triple + "' can't emit an object file (no object-emission support in this build)";
        return false;
    }

    passManager.run(*module);
    outputStream.flush();

    if (outputStream.has_error()) {
        errorMessage = "error writing '" + outputPath + "': " + outputStream.error().message();
        return false;
    }

    return true;
}