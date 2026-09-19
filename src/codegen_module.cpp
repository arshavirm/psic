#include "codegen_state.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

#include <stdexcept>

namespace psi_codegen {

llvm::Type* resolveType(State& s, const TypeNode& type)
{
    auto it = s.llvmTypes.find(type.baseName);
    if (it == s.llvmTypes.end()) {
        psi::ErrorStream() << "unknown type '" << type.baseName << "'\n";
        return nullptr;
    }
    llvm::Type* result = it->second;
    for (int i = 0; i < type.pointerLevel; i++) {
        result = result->getPointerTo();
    }
    return result;
}

bool isUnsignedTypeName(const std::string& baseName)
{
    return baseName == "u8" || baseName == "u16" || baseName == "u32" || baseName == "u64";
}

namespace {

llvm::Constant* buildStringConstant(State& s, llvm::Module& module, const std::string& str)
{
    auto* strConstant = llvm::ConstantDataArray::getString(*s.context, str, true);
    auto* global = new llvm::GlobalVariable(
        module, strConstant->getType(), true,
        llvm::GlobalValue::PrivateLinkage, strConstant, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return global;
}

} // namespace

llvm::Constant* buildConstant(State& s, ValueNode* value, const TypeNode& declaredType, llvm::Module& module)
{
    llvm::Type* targetType = resolveType(s, declaredType);
    if (!targetType || !value) {
        return nullptr;
    }

    if (value->kind == ValueKind::Number) {
        if (targetType->isFloatingPointTy()) {
            double d = value->numberIsFloat ? value->numberAsFloat : (double)value->numberAsInt;
            return llvm::ConstantFP::get(targetType, d);
        }
        if (!targetType->isIntegerTy()) {
            psi::logError("numeric global initializer requires an integer or floating type");
            return nullptr;
        }
        long long i = value->numberIsFloat ? (long long)value->numberAsFloat : value->numberAsInt;
        return llvm::ConstantInt::get(targetType, (uint64_t)i, true);
    }

    if (value->kind == ValueKind::String) {
        if (!targetType->isPointerTy()) {
            psi::logError("string initializer requires a pointer type");
            return nullptr;
        }
        return buildStringConstant(s, module, value->stringValue);
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
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(*s.context, 0));
    }

    if (value->kind == ValueKind::Array) {
        if (declaredType.pointerLevel < 1) {
            psi::ErrorStream() << "array initializer for '" << declaredType.baseName
                               << "' needs a pointer type, e.g. " << declaredType.baseName << "*\n";
            return nullptr;
        }
        TypeNode elementType = declaredType;
        elementType.pointerLevel -= 1;
        llvm::Type* elemLlvmType = resolveType(s, elementType);
        if (!elemLlvmType) {
            return nullptr;
        }

        std::vector<llvm::Constant*> elements;
        for (auto* element : value->arrayValues) {
            llvm::Constant* c = buildConstant(s, element, elementType, module);
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

namespace {

// Shared accessor walk: advances currentType through one accessor. Returns
// false (optionally reporting the reason) when the accessor is invalid for
// the type. Used by both the IR-building address resolver and the plain
// type resolver below so their rules cannot drift apart.
bool stepAccessorTypeNode(const State& s, const RegNode& reg, TypeNode& currentType,
    const AccessorNode& accessor, bool reportErrors)
{
    if (accessor.kind == AccessorKind::Field) {
        if (currentType.pointerLevel != 0) {
            if (reportErrors)
                psi::ErrorStream() << "'.' field access on '" << reg.name
                                   << "' needs a struct value, not a pointer (use '[ ]' to index through a pointer first)\n";
            return false;
        }
        auto structIt = s.structFieldIndex.find(currentType.baseName);
        if (structIt == s.structFieldIndex.end()) {
            if (reportErrors)
                psi::ErrorStream() << "'" << currentType.baseName << "' is not a struct type\n";
            return false;
        }
        auto fieldIt = structIt->second.find(accessor.fieldName);
        if (fieldIt == structIt->second.end()) {
            if (reportErrors)
                psi::ErrorStream() << "struct '" << currentType.baseName
                                   << "' has no field named '" << accessor.fieldName << "'\n";
            return false;
        }
        currentType = s.structFieldTypes.at(currentType.baseName)[fieldIt->second];
        return true;
    }

    if (currentType.pointerLevel < 1) {
        if (reportErrors)
            psi::ErrorStream() << "'[ ]' index on '" << reg.name
                               << "' needs a pointer value (use '.' to access a struct field instead)\n";
        return false;
    }
    currentType.pointerLevel -= 1;
    return true;
}

} // namespace

RegisterAddress resolveRegisterAddress(State& s, const RegNode& reg, llvm::IRBuilder<>* builder)
{
    RegisterAddress result;

    llvm::Value* address = nullptr;
    TypeNode currentType;

    auto localIt = s.locals.find(reg.name);
    if (localIt != s.locals.end()) {
        address = localIt->second;
        currentType = s.localTypes[reg.name];
    } else {
        auto globIt = s.globals.find(reg.name);
        if (globIt != s.globals.end()) {
            address = globIt->second;
            currentType = s.globalTypes[reg.name];
        } else {
            psi::ErrorStream() << "use of undeclared register '" << reg.name << "'\n";
            return result;
        }
    }

    for (const auto& accessor : reg.accessors) {
        const TypeNode typeBefore = currentType;
        if (!stepAccessorTypeNode(s, reg, currentType, accessor, true)) {
            return {};
        }

        if (accessor.kind == AccessorKind::Field) {
            llvm::Type* structLlvmType = resolveType(s, typeBefore);
            if (!structLlvmType) {
                return {};
            }
            address = builder->CreateStructGEP(structLlvmType, address,
                (unsigned)s.structFieldIndex.at(typeBefore.baseName).at(accessor.fieldName));
        } else {
            llvm::Type* pointerLlvmType = resolveType(s, TypeNode { currentType.baseName, currentType.pointerLevel + 1 });
            if (!pointerLlvmType) {
                return {};
            }
            llvm::Value* pointerValue = builder->CreateLoad(pointerLlvmType, address);

            llvm::Type* elementLlvmType = resolveType(s, currentType);
            if (!elementLlvmType) {
                return {};
            }

            address = builder->CreateGEP(elementLlvmType, pointerValue, builder->getInt32(accessor.index));
        }
    }

    result.address = address;
    result.typeNode = currentType;
    return result;
}

bool resolveRegisterTypeNode(const State& s, const RegNode& reg, TypeNode& outType)
{
    TypeNode currentType;

    auto localIt = s.localTypes.find(reg.name);
    if (localIt != s.localTypes.end()) {
        currentType = localIt->second;
    } else {
        auto globIt = s.globalTypes.find(reg.name);
        if (globIt != s.globalTypes.end()) {
            currentType = globIt->second;
        } else {
            return false;
        }
    }

    for (const auto& accessor : reg.accessors) {
        if (!stepAccessorTypeNode(s, reg, currentType, accessor, false)) {
            return false;
        }
    }

    outType = currentType;
    return true;
}

bool isDeclaredUnsigned(const State& s, const ValueNode& value)
{
    if (value.kind != ValueKind::Register) {
        return false;
    }
    TypeNode type;
    if (!resolveRegisterTypeNode(s, value.registerValue, type)) {
        return false;
    }
    return isUnsignedTypeName(type.baseName);
}

std::string defaultTriple(Architecture arch, OperatingSystem os)
{
    const bool wasm = arch == Architecture::WASM32 || arch == Architecture::WASM64;
    if (wasm) {
        if (os != OperatingSystem::FreeStanding && os != OperatingSystem::WASI)
            throw std::runtime_error("WebAssembly requires OS none or wasi");
        return std::string(arch == Architecture::WASM32 ? "wasm32" : "wasm64")
            + (os == OperatingSystem::WASI ? "-unknown-wasi" : "-unknown-unknown");
    }
    if (os == OperatingSystem::WASI)
        throw std::runtime_error("WASI requires WebAssembly");
    std::string cpu;
    switch (arch) {
    case Architecture::RISCV32: cpu = "riscv32"; break;
    case Architecture::RISCV64: cpu = "riscv64"; break;
    case Architecture::PPC32: cpu = "powerpc"; break;
    case Architecture::PPC64: cpu = "powerpc64"; break;
    case Architecture::PPC64LE: cpu = "powerpc64le"; break;
    case Architecture::MIPS: cpu = "mips"; break;
    case Architecture::MIPSEL: cpu = "mipsel"; break;
    case Architecture::MIPS64: cpu = "mips64"; break;
    case Architecture::MIPS64EL: cpu = "mips64el"; break;
    case Architecture::LoongArch64: cpu = "loongarch64"; break;
    case Architecture::SystemZ: cpu = "s390x"; break;
    default: break;
    }
    if (!cpu.empty()) {
        if (os != OperatingSystem::Linux && os != OperatingSystem::FreeStanding)
            throw std::runtime_error("this architecture supports Linux or freestanding targets");
        const bool mips64 = arch == Architecture::MIPS64 || arch == Architecture::MIPS64EL;
        return cpu + (os == OperatingSystem::Linux
            ? (mips64 ? "-unknown-linux-gnuabi64" : "-unknown-linux-gnu") : "-unknown-none");
    }
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
        default: break;
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
        default: break;
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
        default: break;
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
        default: break;
        }
        break;
    default: break;
    }
    throw std::runtime_error("unsupported architecture/OS combination");
}

std::string compileProgram(std::string name, ProgramNode program, Architecture arch, OperatingSystem os, const std::string& targetTriple)
{
    psi::resetErrors();

    State s;
    llvm::LLVMContext context;
    setCodegenDiagnosticHandler(context);
    s.context = &context;
    s.architecture = arch;
    s.os = os;
    buildSpecialRegisterTable(s);

    llvm::Module module(name, context);
    const std::string defaultTarget = defaultTriple(arch, os);
    module.setTargetTriple(targetTriple.empty() ? defaultTarget : targetTriple);
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    std::string targetError;
    auto targetMachine = buildTargetMachine(module.getTargetTriple(), targetError);
    if (!targetMachine) throw std::runtime_error(targetError);
    module.setDataLayout(targetMachine->createDataLayout());

    s.llvmTypes.clear();
    s.llvmTypes["void"] = llvm::Type::getVoidTy(context);
    s.llvmTypes["bool"] = llvm::Type::getInt1Ty(context);
    s.llvmTypes["i8"] = llvm::Type::getInt8Ty(context);
    s.llvmTypes["u8"] = llvm::Type::getInt8Ty(context);
    s.llvmTypes["i16"] = llvm::Type::getInt16Ty(context);
    s.llvmTypes["u16"] = llvm::Type::getInt16Ty(context);
    s.llvmTypes["i32"] = llvm::Type::getInt32Ty(context);
    s.llvmTypes["u32"] = llvm::Type::getInt32Ty(context);
    s.llvmTypes["i64"] = llvm::Type::getInt64Ty(context);
    s.llvmTypes["u64"] = llvm::Type::getInt64Ty(context);
    s.llvmTypes["f32"] = llvm::Type::getFloatTy(context);
    s.llvmTypes["f64"] = llvm::Type::getDoubleTy(context);

    s.llvmTypes["i8x16"] = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(context), 16);
    s.llvmTypes["i16x8"] = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(context), 8);
    s.llvmTypes["i32x4"] = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(context), 4);
    s.llvmTypes["i64x2"] = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(context), 2);
    s.llvmTypes["f32x4"] = llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 4);
    s.llvmTypes["f64x2"] = llvm::FixedVectorType::get(llvm::Type::getDoubleTy(context), 2);

    s.llvmTypes["i32x8"] = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(context), 8);
    s.llvmTypes["i64x4"] = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(context), 4);
    s.llvmTypes["f32x8"] = llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 8);
    s.llvmTypes["f64x4"] = llvm::FixedVectorType::get(llvm::Type::getDoubleTy(context), 4);

    s.functionTypes.clear();
    s.functionDeclarations.clear();
    s.functionParamTypes.clear();
    s.functionReturnTypes.clear();
    s.globals.clear();
    s.globalTypes.clear();
    s.structFieldIndex.clear();
    s.structFieldTypes.clear();

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Struct) {
            const std::string& structName = dec.structDecl.type.baseName;
            if (!s.llvmTypes.count(structName)) {
                s.llvmTypes[structName] = llvm::StructType::create(context, structName);
            }
        }
    }

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Struct) {
            const std::string& structName = dec.structDecl.type.baseName;
            auto* structType = llvm::cast<llvm::StructType>(s.llvmTypes[structName]);
            if (!structType->isOpaque()) {
                continue;
            }

            std::vector<llvm::Type*> fieldLlvmTypes;
            std::vector<TypeNode> fieldTypeNodes;
            std::unordered_map<std::string, int> fieldIndex;

            int idx = 0;
            for (auto& field : dec.structDecl.fields) {
                llvm::Type* fieldType = resolveType(s, field.type);
                fieldLlvmTypes.push_back(fieldType ? fieldType : llvm::Type::getInt8Ty(context));
                fieldTypeNodes.push_back(field.type);
                fieldIndex[field.name] = idx;
                idx++;
            }

            structType->setBody(fieldLlvmTypes);
            s.structFieldTypes[structName] = fieldTypeNodes;
            s.structFieldIndex[structName] = fieldIndex;
        }
    }

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Func) {
            if (s.functionDeclarations.count(dec.funcDecl.name)) {

                continue;
            }

            std::vector<llvm::Type*> argTypes;
            for (auto& arg : dec.funcDecl.args) {
                llvm::Type* argType = resolveType(s, arg.type);
                argTypes.push_back(argType ? argType : llvm::Type::getInt32Ty(context));
            }
            llvm::Type* retType = resolveType(s, dec.funcDecl.returnType);
            if (!retType) {
                retType = llvm::Type::getVoidTy(context);
            }

            auto* fnType = llvm::FunctionType::get(retType, argTypes, false);
            s.functionTypes[dec.funcDecl.name] = fnType;

            std::vector<TypeNode> paramTypes;
            for (auto& arg : dec.funcDecl.args) {
                paramTypes.push_back(arg.type);
            }
            s.functionParamTypes[dec.funcDecl.name] = paramTypes;
            s.functionReturnTypes[dec.funcDecl.name] = dec.funcDecl.returnType;

            auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, dec.funcDecl.name, module);
            size_t idx = 0;
            for (auto& arg : fn->args()) {
                arg.setName(dec.funcDecl.args[idx++].name);
            }
            s.functionDeclarations[dec.funcDecl.name] = fn;
        } else if (dec.kind == DeclKind::Glob || dec.kind == DeclKind::Const) {
            bool isConst = dec.kind == DeclKind::Const;
            const TypeNode& type = isConst ? dec.constDecl.type : dec.globDecl.type;
            const std::string& gname = isConst ? dec.constDecl.name : dec.globDecl.name;
            ValueNode* val = isConst ? dec.constDecl.value : dec.globDecl.value;

            llvm::Type* llvmType = resolveType(s, type);
            if (!llvmType) {
                continue;
            }
            llvm::Constant* init = buildConstant(s, val, type, module);
            if (!init) {
                psi::ErrorStream() << "could not build initializer for '" << gname << "'\n";
                continue;
            }

            auto* global = new llvm::GlobalVariable(
                module, llvmType, isConst, llvm::GlobalValue::ExternalLinkage, init, gname);
            s.globals[gname] = global;
            s.globalTypes[gname] = type;
        }
    }

    for (auto& dec : program.declarations) {
        if (dec.kind == DeclKind::Entry) {
            const std::string& entryName = dec.entryDecl.name;

            if (s.functionDeclarations.count(entryName)) {
                psi::ErrorStream() << "'" << entryName
                                   << "' is already declared as a func - entry needs its own name";
                continue;
            }

            llvm::FunctionType* entry_function_type = llvm::FunctionType::get(s.llvmTypes["void"], { }, false);
            llvm::Function* entry_function = llvm::Function::Create(
                entry_function_type, llvm::Function::ExternalLinkage, entryName, module);

            generateFunctionBody(s, dec.entryDecl.body, entry_function, nullptr, entryName);
        } else if (dec.kind == DeclKind::Func) {
            llvm::Function* fn = s.functionDeclarations[dec.funcDecl.name];
            if (dec.funcDecl.hasBody) {
                generateFunctionBody(s, dec.funcDecl.body, fn, &dec.funcDecl.args, dec.funcDecl.name);
            }
        }
    }

    // Preserve baseline ISA requirements when users pass emitted IR to LLVM tools.
    for (auto& function : module) {
        if (!function.isDeclaration() && !targetMachine->getTargetFeatureString().empty())
            function.addFnAttr("target-features", targetMachine->getTargetFeatureString());
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

} // namespace psi_codegen
