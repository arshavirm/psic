#pragma once

#include <string>
#include <memory>
#include <vector>

struct TypeNode {
    std::string baseName;
    int pointerLevel = 0;
    bool hasAlignment = false;
    int alignment = 0;
    bool isPrimitive = false;
};

enum class AccessorKind {
    Index,
    Field,
};

struct AccessorNode {
    AccessorKind kind = AccessorKind::Index;
    int index = 0;
    std::string fieldName;
};

struct RegNode {
    std::string name;
    std::vector<AccessorNode> accessors;
};

struct SpecialRegNode {
    std::string name;
};

struct InstNode {
    std::string name;
    bool isSpecial = false;
};

enum class ValueKind {
    Number,
    String,
    Register,
    Array,
    SpecialRegister,
    Bool,
    Null,
};

struct ValueNode {
    ValueKind kind = ValueKind::Number;

    bool numberIsFloat = false;
    long long numberAsInt = 0;
    double numberAsFloat = 0.0;

    bool boolValue = false;

    std::string stringValue;

    RegNode registerValue;

    std::vector<ValueNode*> arrayValues;

    SpecialRegNode specialRegisterValue;
};

struct ArgNode {
    TypeNode type;
    std::string name;
};

enum class TargetKind {
    None,
    Register,
    SpecialRegister,
};

struct CommandNode {
    bool isEmpty = false;

    bool hasDeclaredType = false;
    TypeNode declaredType;

    TargetKind targetKind = TargetKind::None;
    RegNode targetRegister;
    SpecialRegNode targetSpecialRegister;

    bool hasInstruction = false;
    InstNode instruction;

    std::vector<ValueNode*> values;
};

struct BlockNode {
    std::vector<CommandNode> commands;
};

struct StructDeclNode {
    TypeNode type;
    std::vector<ArgNode> fields;
};

struct FuncDeclNode {
    TypeNode returnType;
    std::string name;
    std::vector<ArgNode> args;
    bool hasBody = false;
    BlockNode body;
};

struct GlobDeclNode {
    TypeNode type;
    std::string name;
    ValueNode* value = nullptr;
};

struct ConstDeclNode {
    TypeNode type;
    std::string name;
    ValueNode* value = nullptr;
};

struct EntryDeclNode {
    std::string name;
    BlockNode body;
};

enum class DeclKind {
    Entry,
    Struct,
    Func,
    Glob,
    Const,
};

struct DeclarationNode {
    DeclKind kind = DeclKind::Glob;
    EntryDeclNode entryDecl;
    StructDeclNode structDecl;
    FuncDeclNode funcDecl;
    GlobDeclNode globDecl;
    ConstDeclNode constDecl;
};

struct ProgramNode {
    std::vector<DeclarationNode> declarations;
    // Nodes refer to values by pointer; this arena owns them. Keeping ownership
    // on the program lets AST copies used by lowering share the same values.
    std::vector<std::shared_ptr<ValueNode>> ownedValues;
};
