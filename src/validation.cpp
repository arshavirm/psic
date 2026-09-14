#include "validation.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
void validateType(const TypeNode& type, bool allowVoid = false)
{
    require(allowVoid || type.baseName != "void" || type.pointerLevel > 0,
        "void is only valid as a return type or pointer element");
    if (type.hasAlignment)
        require(type.alignment > 0 && (type.alignment & (type.alignment - 1)) == 0,
            "alignment must be a positive power of two");
}
void validateBlock(const BlockNode& block, std::unordered_set<std::string> locals = {})
{
    const std::unordered_set<std::string> binary = {
        "add", "sub", "mul", "div", "mod", "eq", "gt", "lt", "neq", "gte", "lte",
        "and", "or", "xor", "lsh", "rsh", "land", "lor"};
    std::unordered_set<std::string> labels;
    for (const auto& cmd : block.commands) {
        if (cmd.isEmpty) continue;
        if (cmd.hasDeclaredType) {
            validateType(cmd.declaredType);
            require(locals.insert(cmd.targetRegister.name).second,
                "duplicate local declaration '" + cmd.targetRegister.name + "'");
        }
        if (!cmd.hasInstruction) {
            require(cmd.hasDeclaredType || !cmd.values.empty(), "assignment requires a value");
            continue;
        }
        if (cmd.instruction.isSpecial) continue;
        const auto& op = cmd.instruction.name;
        const auto count = cmd.values.size();
        int expected = -1;
        if (binary.count(op) || op == "store" || op == "cjmp") expected = 2;
        if (op == "not" || op == "lnot" || op == "load" || op == "ref" || op == "jmp" || op == "label") expected = 1;
        require(expected < 0 || count == static_cast<size_t>(expected),
            "'" + op + "' requires " + std::to_string(expected) + " operand(s)");
        if (op == "ret") require(count <= 1, "'ret' takes at most one operand");
        if (op == "call") require(count >= 1, "'call' requires a function name");
        if (op == "ret" || op == "store" || op == "jmp" || op == "cjmp" || op == "label")
            require(cmd.targetKind == TargetKind::None, "'" + op + "' cannot have a result target");
        else if (op != "call")
            require(cmd.targetKind != TargetKind::None, "'" + op + "' requires a result target");
        if (op == "load") require(cmd.hasDeclaredType, "'load' requires an explicit result type");
        if (op == "label" || op == "jmp" || op == "cjmp" || op == "call" || op == "ref") {
            const auto* value = cmd.values[op == "cjmp" ? 1 : 0];
            require(value->kind == ValueKind::Register, "'" + op + "' requires a named operand");
            if (op != "ref") require(value->registerValue.accessors.empty(), "'" + op + "' requires a plain name");
            if (op == "label") require(labels.insert(value->registerValue.name).second,
                "duplicate label '" + value->registerValue.name + "'");
        }
    }
}
}

void validateProgram(const ProgramNode& program)
{
    const std::unordered_set<std::string> builtinTypes = {
        "void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
        "i8x16", "i16x8", "i32x4", "i64x2", "f32x4", "f64x2", "i32x8", "i64x4", "f32x8", "f64x4"};
    std::unordered_set<std::string> symbols;
    std::unordered_map<std::string, const StructDeclNode*> structs;
    for (const auto& decl : program.declarations) {
        std::string name;
        switch (decl.kind) {
        case DeclKind::Entry: name = decl.entryDecl.name; validateBlock(decl.entryDecl.body); break;
        case DeclKind::Func: {
            const auto& fn = decl.funcDecl;
            name = fn.name;
            validateType(fn.returnType, true);
            std::unordered_set<std::string> args;
            for (const auto& arg : fn.args) {
                validateType(arg.type);
                require(args.insert(arg.name).second, "duplicate argument '" + arg.name + "'");
            }
            validateBlock(fn.body, args);
            break;
        }
        case DeclKind::Struct: {
            const auto& st = decl.structDecl;
            name = st.type.baseName;
            require(!builtinTypes.count(name), "struct cannot redefine builtin type '" + name + "'");
            require(st.type.pointerLevel == 0, "struct name cannot be a pointer type");
            structs[name] = &st;
            std::unordered_set<std::string> fields;
            for (const auto& field : st.fields) {
                validateType(field.type);
                require(fields.insert(field.name).second, "duplicate field '" + field.name + "'");
            }
            break;
        }
        case DeclKind::Glob: name = decl.globDecl.name; validateType(decl.globDecl.type); break;
        case DeclKind::Const: name = decl.constDecl.name; validateType(decl.constDecl.type); break;
        }
        require(symbols.insert(name).second, "duplicate declaration '" + name + "'");
    }
    // By-value struct cycles cannot have a finite LLVM size; pointer cycles are fine.
    std::unordered_set<std::string> visiting, visited;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        if (visited.count(name) || !structs.count(name)) return;
        require(visiting.insert(name).second, "recursive by-value struct '" + name + "'");
        for (const auto& field : structs.at(name)->fields)
            if (field.type.pointerLevel == 0) visit(field.type.baseName);
        visiting.erase(name);
        visited.insert(name);
    };
    for (const auto& item : structs) visit(item.first);
}
