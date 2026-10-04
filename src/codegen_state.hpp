#pragma once

// Internal codegen state shared between the codegen translation units.
// This header is not installed and is not part of the public library API.

#include "codegen.hpp"
#include "ast.hpp"
#include "logging.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace psi_codegen {

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
    std::string readAsm;
    std::string writeAsm;
};

// All mutable state for one compileProgram call. Every lowering function
// takes a reference to it, so nothing depends on hidden global state and
// the data flow between translation units is explicit.
struct State {
    llvm::LLVMContext* context = nullptr;
    Architecture architecture = Architecture::X86_64;
    OperatingSystem os = OperatingSystem::Linux;
    std::string targetTriple;
    std::string targetFeatures;

    llvm::Function* currentFunction = nullptr;
    TypeNode currentFunctionReturnType;

    std::unordered_map<std::string, llvm::Type*> llvmTypes;
    std::unordered_map<std::string, llvm::Function*> functionDeclarations;
    std::unordered_map<std::string, std::vector<TypeNode>> functionParamTypes;
    std::unordered_map<std::string, TypeNode> functionReturnTypes;

    std::unordered_map<std::string, llvm::GlobalVariable*> globals;
    std::unordered_map<std::string, TypeNode> globalTypes;

    std::unordered_map<std::string, std::unordered_map<std::string, int>> structFieldIndex;
    std::unordered_map<std::string, std::vector<TypeNode>> structFieldTypes;

    std::unordered_map<std::string, llvm::Value*> locals;
    std::unordered_map<std::string, llvm::Value*> localStorage;
    std::unordered_map<std::string, TypeNode> localTypes;
    std::unordered_map<std::string, llvm::BasicBlock*> labels;
    std::vector<std::string> declaredLocalNames;

    std::unordered_map<std::string, SpecialRegisterInfo> specialRegisters;
};

inline bool isAArch64(Architecture arch) { return arch == Architecture::AArch64; }
inline bool isARM32(Architecture arch) { return arch == Architecture::ARM; }
inline bool isARM(Architecture arch) { return isARM32(arch) || isAArch64(arch); }
inline bool isWasm(const State& s)
{
    return s.architecture == Architecture::WASM32 || s.architecture == Architecture::WASM64;
}

// --- codegen_module.cpp ---
llvm::Type* resolveType(State& s, const TypeNode& type);
bool isUnsignedTypeName(const std::string& baseName);
std::string defaultTriple(Architecture arch, OperatingSystem os);
llvm::Constant* buildConstant(State& s, const ValueNode* value, const TypeNode& declaredType, llvm::Module& module);

struct RegisterAddress {
    llvm::Value* address = nullptr;
    TypeNode typeNode;
};
void storeValueWithZeroedPadding(llvm::IRBuilder<>& builder, llvm::Value* value,
    llvm::Value* address);
llvm::Value* loadValueBytewiseAtomic(llvm::IRBuilder<>& builder, llvm::Type* type,
    llvm::Value* address, bool isVolatile = false);
void storeValueBytewiseAtomic(llvm::IRBuilder<>& builder, llvm::Value* value,
    llvm::Value* address, bool isVolatile = false);
void trapIfCondition(llvm::Value* condition, const char* blockPrefix,
    llvm::IRBuilder<>* builder);
void trapIfNullPointer(llvm::Value* pointer, llvm::IRBuilder<>* builder);
void trapIfMisalignedPointer(llvm::Value* pointer, llvm::Align alignment,
    llvm::IRBuilder<>* builder);
void trapIfAddressRangeWraps(llvm::Value* pointer, llvm::Value* byteCount,
    llvm::IRBuilder<>* builder);
void trapIfAccessRangeWraps(llvm::Value* pointer, llvm::Type* accessType,
    llvm::IRBuilder<>* builder);
RegisterAddress resolveRegisterAddress(State& s, const RegNode& reg,
    llvm::IRBuilder<>* builder, bool dereferenceFinal = true);
bool resolveRegisterTypeNode(const State& s, const RegNode& reg, TypeNode& outType);
bool isDeclaredUnsigned(const State& s, const ValueNode& value);

// --- codegen_registers.cpp ---
void buildSpecialRegisterTable(State& s);
llvm::Type* widthToType(RegisterWidth width, llvm::LLVMContext& context);
llvm::Value* wasmMemorySize(State& s, llvm::IRBuilder<>* builder);
llvm::Value* processSpecialRegisterRead(State& s, const SpecialRegNode& reg, llvm::IRBuilder<>* builder);
void processSpecialRegisterWrite(State& s, const SpecialRegNode& reg, llvm::Value* value, llvm::IRBuilder<>* builder);

// --- codegen_instructions.cpp ---
llvm::Value* processValue(State& s, const ValueNode& value, llvm::IRBuilder<>* builder);
llvm::Value* coerceValue(llvm::Value* value, llvm::Type* targetType, llvm::IRBuilder<>* builder,
    const std::string& context, bool treatSourceAsUnsigned = false);
llvm::Value* computeCommandValue(State& s, const CommandNode& command, llvm::IRBuilder<>* builder,
    const TypeNode* declaredType);
void processCommand(State& s, const CommandNode& command, llvm::IRBuilder<>* builder);
void generateFunctionBody(State& s, const BlockNode& body, llvm::Function* function,
    const std::vector<ArgNode>* args, const std::string& diagnosticName);

// --- codegen.cpp (target / emission) ---
std::unique_ptr<llvm::TargetMachine> buildTargetMachine(const std::string& triple, std::string& errorMessage,
    const std::string& cpu = "", const std::string& requestedFeatures = "");

// Installs the handler that routes LLVM diagnostics into the psi log.
void setCodegenDiagnosticHandler(llvm::LLVMContext& context);

} // namespace psi_codegen
