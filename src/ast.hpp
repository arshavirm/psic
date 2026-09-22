#pragma once

#include <string>
#include <memory>
#include <vector>

/**
 * AST Node Types for PSI Language
 * 
 * This header defines the abstract syntax tree (AST) nodes used by the PSI parser.
 * The AST represents the hierarchical structure of a PSI program after lexical analysis.
 */

/**
 * Represents a type annotation in PSI source code.
 * Types can be primitive (i32, f64, etc.) or user-defined structs, with optional
 * pointer indirection and alignment specifications.
 */
struct TypeNode {
    std::string baseName;      ///< Base type name (e.g., "i32", "f64", struct name)
    int pointerLevel = 0;      ///< Number of pointer indirections (0 = direct type)
    bool hasAlignment = false; ///< Whether explicit alignment is specified
    int alignment = 0;         ///< Alignment requirement (power of 2, if specified)
    bool isPrimitive = false;  ///< True if this is a built-in primitive type
};

/**
 * Kinds of accessor operations on composite values.
 */
enum class AccessorKind {
    Index,  ///< Array indexing: reg[index]
    Field,  ///< Struct field access: reg.field
};

/**
 * Represents an accessor operation (indexing or field access).
 */
struct AccessorNode {
    AccessorKind kind = AccessorKind::Index; ///< Type of accessor
    int index = 0;                           ///< Array index (for Index kind)
    std::string fieldName;                   ///< Field name (for Field kind)
};

/**
 * Represents a physical or virtual register reference.
 * Registers can have chained accessors for array/struct member access.
 */
struct RegNode {
    std::string name;                        ///< Register name (e.g., "rax", "x0")
    std::vector<AccessorNode> accessors;     ///< Chained accessors (e.g., arr[0].field)
};

/**
 * Represents a special system register (target-specific).
 * Examples: %cycle, %mxcsr, %memory_pages
 */
struct SpecialRegNode {
    std::string name; ///< Special register name (without % prefix)
};

/**
 * Represents an instruction (standard or target-specific).
 */
struct InstNode {
    std::string name;      ///< Instruction name (e.g., "add", "memory.size")
    bool isSpecial = false; ///< True if this is a target-specific #instruction
};

/**
 * Kinds of literal or computed values in PSI.
 */
enum class ValueKind {
    Number,         ///< Numeric literal (integer or float)
    String,         ///< String literal
    Register,       ///< Register reference
    Array,          ///< Array literal [...]
    SpecialRegister,///< Special register (%name)
    Bool,           ///< Boolean literal (true/false)
    Null,           ///< Null pointer literal
};

/**
 * Represents a value node in the AST.
 * Uses a tagged union pattern - only the field matching 'kind' is valid.
 */
struct ValueNode {
    ValueKind kind = ValueKind::Number; ///< Discriminant for union fields

    // Number fields
    bool numberIsFloat = false;  ///< True if number is floating-point
    long long numberAsInt = 0;   ///< Integer value
    double numberAsFloat = 0.0;  ///< Floating-point value

    bool boolValue = false;      ///< Boolean value

    std::string stringValue;     ///< String content (for String kind)
    RegNode registerValue;       ///< Register reference (for Register kind)
    std::vector<ValueNode*> arrayValues; ///< Array elements (for Array kind)
    SpecialRegNode specialRegisterValue; ///< Special register (for SpecialRegister kind)
};

/**
 * Represents a function or struct field argument.
 */
struct ArgNode {
    TypeNode type;   ///< Argument type
    std::string name;///< Argument name
};

/**
 * Kinds of assignment targets in commands.
 */
enum class TargetKind {
    None,            ///< No target (e.g., standalone expression)
    Register,        ///< Virtual register assignment
    SpecialRegister, ///< Special register assignment
};

/**
 * Represents a single command/statement within a basic block.
 * Commands can be assignments, instructions, or control flow.
 */
struct CommandNode {
    bool isEmpty = false;              ///< True if this is just a semicolon

    bool hasDeclaredType = false;      ///< True if variable has explicit type
    TypeNode declaredType;             ///< Declared type (if hasDeclaredType)

    TargetKind targetKind = TargetKind::None; ///< Kind of assignment target
    RegNode targetRegister;            ///< Target register (for Register kind)
    SpecialRegNode targetSpecialRegister; ///< Target special register

    bool hasInstruction = false;       ///< True if this command has an instruction
    InstNode instruction;              ///< Instruction (if hasInstruction)

    std::vector<ValueNode*> values;    ///< Instruction operands
};

/**
 * Represents a basic block - a sequence of commands.
 */
struct BlockNode {
    std::vector<CommandNode> commands; ///< Commands in execution order
};

/**
 * Represents a struct type declaration.
 */
struct StructDeclNode {
    TypeNode type;                     ///< Struct type info
    std::vector<ArgNode> fields;       ///< Field declarations
};

/**
 * Represents a function declaration (with or without body).
 */
struct FuncDeclNode {
    TypeNode returnType;               ///< Function return type
    std::string name;                  ///< Function name
    std::vector<ArgNode> args;         ///< Parameter list
    bool hasBody = false;              ///< True if function has implementation
    BlockNode body;                    ///< Function body (if hasBody)
};

/**
 * Represents a global variable declaration.
 */
struct GlobDeclNode {
    TypeNode type;                     ///< Variable type
    std::string name;                  ///< Variable name
    ValueNode* value = nullptr;        ///< Initializer value
};

/**
 * Represents a compile-time constant declaration.
 */
struct ConstDeclNode {
    TypeNode type;                     ///< Constant type
    std::string name;                  ///< Constant name
    ValueNode* value = nullptr;        ///< Constant value
};

/**
 * Represents an entry point declaration (like main).
 */
struct EntryDeclNode {
    std::string name;                  ///< Entry point name
    BlockNode body;                    ///< Entry point body
};

/**
 * Kinds of top-level declarations in PSI.
 */
enum class DeclKind {
    Entry,  ///< Entry point (entry name { ... })
    Struct, ///< Struct type (struct name { ... })
    Func,   ///< Function (func ret name(args) { ... } or func ret name(args);)
    Glob,   ///< Global variable (type name = value;)
    Const,  ///< Constant (const type name = value;)
};

/**
 * Represents any top-level declaration.
 * Uses a variant pattern - only the field matching 'kind' is valid.
 */
struct DeclarationNode {
    DeclKind kind = DeclKind::Glob; ///< Discriminant for union fields
    EntryDeclNode entryDecl;        ///< Entry declaration (for Entry kind)
    StructDeclNode structDecl;      ///< Struct declaration (for Struct kind)
    FuncDeclNode funcDecl;          ///< Function declaration (for Func kind)
    GlobDeclNode globDecl;          ///< Global declaration (for Glob kind)
    ConstDeclNode constDecl;        ///< Constant declaration (for Const kind)
};

/**
 * Root AST node representing a complete PSI program.
 */
struct ProgramNode {
    std::vector<DeclarationNode> declarations; ///< Top-level declarations
    
    /**
     * Owned storage for dynamically allocated ValueNodes.
     * This avoids manual memory management and ensures proper cleanup.
     */
    std::vector<std::shared_ptr<ValueNode>> ownedValues;
};