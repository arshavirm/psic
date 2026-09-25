#include "codegen_state.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

#include <stdexcept>
#include <utility>

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

llvm::Constant* buildConstant(State& s, const ValueNode* value, const TypeNode& declaredType, llvm::Module& module)
{
    llvm::Type* targetType = resolveType(s, declaredType);
    if (!targetType || !value) {
        return nullptr;
    }

    switch (value->kind) {
    case ValueKind::Number: {
        if (targetType->isFloatingPointTy()) {
            const double number = value->numberIsFloat
                ? value->numberAsFloat : static_cast<double>(value->numberAsInt);
            return llvm::ConstantFP::get(targetType, number);
        }
        if (!targetType->isIntegerTy()) {
            psi::logError("numeric global initializer requires an integer or floating type");
            return nullptr;
        }
        const long long integer = value->numberIsFloat
            ? static_cast<long long>(value->numberAsFloat) : value->numberAsInt;
        return llvm::ConstantInt::get(targetType, static_cast<uint64_t>(integer), true);
    }
    case ValueKind::String:
        if (!targetType->isPointerTy()) {
            psi::logError("string initializer requires a pointer type");
            return nullptr;
        }
        return buildStringConstant(s, module, value->stringValue);
    case ValueKind::Bool:
        if (!targetType->isIntegerTy()) {
            psi::ErrorStream() << "a bool literal needs an integer (e.g. bool/i8/i32) type\n";
            return nullptr;
        }
        return llvm::ConstantInt::get(targetType, value->boolValue ? 1 : 0);
    case ValueKind::Null:
        if (declaredType.pointerLevel < 1) {
            psi::ErrorStream() << "'null' needs a pointer type, e.g. " << declaredType.baseName << "*\n";
            return nullptr;
        }
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(*s.context, 0));
    case ValueKind::Array: {
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
    case ValueKind::Register:
    case ValueKind::SpecialRegister:
        break;
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

const TypeNode* findRegisterType(const State& state, const std::string& name)
{
    auto local = state.localTypes.find(name);
    if (local != state.localTypes.end()) return &local->second;

    auto global = state.globalTypes.find(name);
    return global == state.globalTypes.end() ? nullptr : &global->second;
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
    } else {
        auto globIt = s.globals.find(reg.name);
        if (globIt != s.globals.end()) {
            address = globIt->second;
        } else {
            psi::ErrorStream() << "use of undeclared register '" << reg.name << "'\n";
            return result;
        }
    }

    const TypeNode* registerType = findRegisterType(s, reg.name);
    if (!registerType) {
        psi::ErrorStream() << "use of undeclared register '" << reg.name << "'\n";
        return result;
    }
    currentType = *registerType;

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
    const TypeNode* registerType = findRegisterType(s, reg.name);
    if (!registerType) return false;
    TypeNode currentType = *registerType;

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

namespace {

void declareBuiltinTypes(State& state, llvm::LLVMContext& context)
{
    auto addType = [&state](const char* name, llvm::Type* type) {
        state.llvmTypes[name] = type;
    };

    addType("void", llvm::Type::getVoidTy(context));
    addType("bool", llvm::Type::getInt1Ty(context));

    llvm::Type* i8 = llvm::Type::getInt8Ty(context);
    llvm::Type* i16 = llvm::Type::getInt16Ty(context);
    llvm::Type* i32 = llvm::Type::getInt32Ty(context);
    llvm::Type* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* f32 = llvm::Type::getFloatTy(context);
    llvm::Type* f64 = llvm::Type::getDoubleTy(context);

    addType("i8", i8);
    addType("u8", i8);
    addType("i16", i16);
    addType("u16", i16);
    addType("i32", i32);
    addType("u32", i32);
    addType("i64", i64);
    addType("u64", i64);
    addType("f32", f32);
    addType("f64", f64);

    addType("i8x16", llvm::FixedVectorType::get(i8, 16));
    addType("i16x8", llvm::FixedVectorType::get(i16, 8));
    addType("i32x4", llvm::FixedVectorType::get(i32, 4));
    addType("i64x2", llvm::FixedVectorType::get(i64, 2));
    addType("f32x4", llvm::FixedVectorType::get(f32, 4));
    addType("f64x2", llvm::FixedVectorType::get(f64, 2));
    addType("i32x8", llvm::FixedVectorType::get(i32, 8));
    addType("i64x4", llvm::FixedVectorType::get(i64, 4));
    addType("f32x8", llvm::FixedVectorType::get(f32, 8));
    addType("f64x4", llvm::FixedVectorType::get(f64, 4));
}

void declareStructTypes(State& state, const ProgramNode& program, llvm::LLVMContext& context)
{
    for (const auto& declaration : program.declarations) {
        if (declaration.kind != DeclKind::Struct) continue;
        const std::string& name = declaration.structDecl.type.baseName;
        if (!state.llvmTypes.count(name))
            state.llvmTypes[name] = llvm::StructType::create(context, name);
    }

    for (const auto& declaration : program.declarations) {
        if (declaration.kind != DeclKind::Struct) continue;
        const auto& structure = declaration.structDecl;
        const std::string& name = structure.type.baseName;
        auto* llvmStructure = llvm::cast<llvm::StructType>(state.llvmTypes[name]);
        if (!llvmStructure->isOpaque()) continue;

        std::vector<llvm::Type*> fieldTypes;
        std::vector<TypeNode> fieldTypeNodes;
        std::unordered_map<std::string, int> fieldIndexes;
        for (const auto& field : structure.fields) {
            llvm::Type* fieldType = resolveType(state, field.type);
            fieldTypes.push_back(fieldType ? fieldType : llvm::Type::getInt8Ty(context));
            fieldTypeNodes.push_back(field.type);
            fieldIndexes[field.name] = static_cast<int>(fieldIndexes.size());
        }

        llvmStructure->setBody(fieldTypes);
        state.structFieldTypes[name] = std::move(fieldTypeNodes);
        state.structFieldIndex[name] = std::move(fieldIndexes);
    }
}

void declareFunctionsAndGlobals(State& state, const ProgramNode& program,
    llvm::LLVMContext& context, llvm::Module& module)
{
    for (const auto& declaration : program.declarations) {
        if (declaration.kind == DeclKind::Func) {
            const auto& function = declaration.funcDecl;
            if (state.functionDeclarations.count(function.name)) continue;

            std::vector<llvm::Type*> argumentTypes;
            std::vector<TypeNode> parameterTypes;
            for (const auto& argument : function.args) {
                llvm::Type* argumentType = resolveType(state, argument.type);
                argumentTypes.push_back(argumentType ? argumentType : llvm::Type::getInt32Ty(context));
                parameterTypes.push_back(argument.type);
            }

            llvm::Type* returnType = resolveType(state, function.returnType);
            if (!returnType) returnType = llvm::Type::getVoidTy(context);

            auto* functionType = llvm::FunctionType::get(returnType, argumentTypes, false);
            state.functionParamTypes[function.name] = std::move(parameterTypes);
            state.functionReturnTypes[function.name] = function.returnType;

            auto* llvmFunction = llvm::Function::Create(functionType,
                llvm::Function::ExternalLinkage, function.name, module);
            std::size_t argumentIndex = 0;
            for (auto& llvmArgument : llvmFunction->args())
                llvmArgument.setName(function.args[argumentIndex++].name);
            state.functionDeclarations[function.name] = llvmFunction;
            continue;
        }

        if (declaration.kind != DeclKind::Glob && declaration.kind != DeclKind::Const)
            continue;

        const bool isConstant = declaration.kind == DeclKind::Const;
        const TypeNode& type = isConstant ? declaration.constDecl.type : declaration.globDecl.type;
        const std::string& name = isConstant ? declaration.constDecl.name : declaration.globDecl.name;
        const ValueNode* value = isConstant ? declaration.constDecl.value : declaration.globDecl.value;

        llvm::Type* llvmType = resolveType(state, type);
        if (!llvmType) continue;
        llvm::Constant* initializer = buildConstant(state, value, type, module);
        if (!initializer) {
            psi::ErrorStream() << "could not build initializer for '" << name << "'\n";
            continue;
        }

        auto* global = new llvm::GlobalVariable(module, llvmType, isConstant,
            llvm::GlobalValue::ExternalLinkage, initializer, name);
        state.globals[name] = global;
        state.globalTypes[name] = type;
    }
}

void generateProgramBodies(State& state, const ProgramNode& program, llvm::Module& module)
{
    for (const auto& declaration : program.declarations) {
        if (declaration.kind == DeclKind::Entry) {
            const std::string& entryName = declaration.entryDecl.name;
            if (state.functionDeclarations.count(entryName)) {
                psi::ErrorStream() << "'" << entryName
                                   << "' is already declared as a func - entry needs its own name";
                continue;
            }

            auto* functionType = llvm::FunctionType::get(state.llvmTypes["void"], {}, false);
            auto* function = llvm::Function::Create(functionType,
                llvm::Function::ExternalLinkage, entryName, module);
            generateFunctionBody(state, declaration.entryDecl.body, function, nullptr, entryName);
            continue;
        }

        if (declaration.kind != DeclKind::Func || !declaration.funcDecl.hasBody) continue;
        llvm::Function* function = state.functionDeclarations[declaration.funcDecl.name];
        generateFunctionBody(state, declaration.funcDecl.body, function,
            &declaration.funcDecl.args, declaration.funcDecl.name);
    }
}

} // namespace

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
    std::string targetError;
    auto targetMachine = buildTargetMachine(module.getTargetTriple(), targetError);
    if (!targetMachine) throw std::runtime_error(targetError);
    module.setDataLayout(targetMachine->createDataLayout());

    declareBuiltinTypes(s, context);
    declareStructTypes(s, program, context);
    declareFunctionsAndGlobals(s, program, context, module);
    generateProgramBodies(s, program, module);

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
