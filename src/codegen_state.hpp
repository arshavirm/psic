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
RegisterAddress resolveRegisterAddress(State& s, const RegNode& reg, llvm::IRBuilder<>* builder);
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
std::unique_ptr<llvm::TargetMachine> buildTargetMachine(const std::string& triple, std::string& errorMessage);

// Installs the handler that routes LLVM diagnostics into the psi log.
void setCodegenDiagnosticHandler(llvm::LLVMContext& context);

} // namespace psi_codegen
