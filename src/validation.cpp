#include "validation.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr std::array<std::string_view, 18> binaryInstructions = {
    "add", "sub", "mul", "div", "mod", "eq", "gt", "lt", "neq", "gte", "lte",
    "and", "or", "xor", "lsh", "rsh", "land", "lor"};
constexpr std::array<std::string_view, 6> unaryInstructions = {
    "not", "lnot", "load", "ref", "jmp", "label"};

constexpr std::array<std::string_view, 22> builtinTypes = {
    "void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
    "i8x16", "i16x8", "i32x4", "i64x2", "f32x4", "f64x2", "i32x8", "i64x4", "f32x8", "f64x4"};

template <std::size_t Size>
bool contains(const std::array<std::string_view, Size>& values, const std::string& value)
{
    for (std::string_view candidate : values)
        if (candidate == value)
            return true;
    return false;
}

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

std::optional<std::size_t> expectedOperandCount(const std::string& op)
{
    if (contains(binaryInstructions, op) || op == "store" || op == "cjmp") return 2;
    if (contains(unaryInstructions, op)) return 1;
    return std::nullopt;
}

void validateInstruction(const CommandNode& command, std::unordered_set<std::string>& labels)
{
    if (command.instruction.isSpecial) return;

    const std::string& op = command.instruction.name;
    const std::size_t count = command.values.size();
    if (const auto expected = expectedOperandCount(op)) {
        require(count == *expected,
            "'" + op + "' requires " + std::to_string(*expected) + " operand(s)");
    }
    if (op == "ret") require(count <= 1, "'ret' takes at most one operand");
    if (op == "call") require(count >= 1, "'call' requires a function name");

    const bool cannotHaveTarget = op == "ret" || op == "store" || op == "jmp"
        || op == "cjmp" || op == "label";
    if (cannotHaveTarget) {
        require(command.targetKind == TargetKind::None, "'" + op + "' cannot have a result target");
    } else if (op != "call") {
        require(command.targetKind != TargetKind::None, "'" + op + "' requires a result target");
    }
    if (op == "load") require(command.hasDeclaredType, "'load' requires an explicit result type");

    const bool needsNamedOperand = op == "label" || op == "jmp" || op == "cjmp"
        || op == "call" || op == "ref";
    if (!needsNamedOperand) return;

    const std::size_t operandIndex = op == "cjmp" ? 1 : 0;
    const ValueNode* value = command.values[operandIndex];
    require(value->kind == ValueKind::Register, "'" + op + "' requires a named operand");
    if (op != "ref")
        require(value->registerValue.accessors.empty(), "'" + op + "' requires a plain name");
    if (op == "label")
        require(labels.insert(value->registerValue.name).second,
            "duplicate label '" + value->registerValue.name + "'");
}

struct TypeEnvironment {
    std::unordered_map<std::string, TypeNode> globals;
    std::unordered_map<std::string, TypeNode> returns;
    std::unordered_map<std::string, std::vector<TypeNode>> parameters;
    std::unordered_map<std::string, const StructDeclNode*> structs;
};

bool sameType(const TypeNode& left, const TypeNode& right)
{
    return left.baseName == right.baseName && left.pointerLevel == right.pointerLevel;
}

TypeNode registerType(const RegNode& reg,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env)
{
    auto local = locals.find(reg.name);
    auto global = env.globals.find(reg.name);
    require(local != locals.end() || global != env.globals.end(),
        "use of undeclared register '" + reg.name + "'");
    TypeNode type = local != locals.end() ? local->second : global->second;
    for (const auto& accessor : reg.accessors) {
        if (accessor.kind == AccessorKind::Index) {
            require(type.pointerLevel > 0, "index accessor requires a pointer");
            --type.pointerLevel;
            continue;
        }
        require(type.pointerLevel == 0, "field accessor requires a struct value");
        auto structure = env.structs.find(type.baseName);
        require(structure != env.structs.end(), "field accessor requires a struct type");
        auto field = std::find_if(structure->second->fields.begin(), structure->second->fields.end(),
            [&](const ArgNode& candidate) { return candidate.name == accessor.fieldName; });
        require(field != structure->second->fields.end(),
            "struct '" + type.baseName + "' has no field named '" + accessor.fieldName + "'");
        type = field->type;
    }
    return type;
}

struct ValueType {
    TypeNode type;
    bool isNull = false;
    bool known = false;
};

ValueType inferValueType(const ValueNode& value,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env)
{
    switch (value.kind) {
    case ValueKind::Number: return {{value.numberIsFloat ? "f32" : "i32", 0}, false, true};
    case ValueKind::Bool: return {{"bool", 0}, false, true};
    case ValueKind::String: return {{"i8", 1}, false, true};
    case ValueKind::Null: return {{}, true, true};
    case ValueKind::Register: return {registerType(value.registerValue, locals, env), false, true};
    case ValueKind::Array:
    case ValueKind::SpecialRegister: return {};
    }
    return {};
}

ValueType inferCommandType(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env)
{
    if (command.instruction.isSpecial) {
        std::string name = command.instruction.name;
        std::replace(name.begin(), name.end(), '.', '_');
        if (name == "ptrcast" || name == "inttoptr" || name == "atomicload" || name == "vsplat")
            return {command.declaredType, false, command.hasDeclaredType};
        if (name == "ptrtoint") return {command.declaredType, false, command.hasDeclaredType};
        if ((name.rfind("atomic", 0) == 0 || name == "cas") && !command.values.empty()
            && command.values[0]->kind == ValueKind::Register) {
            TypeNode type = registerType(command.values[0]->registerValue, locals, env);
            if (type.pointerLevel > 0) --type.pointerLevel;
            return {type, false, true};
        }
        if (!command.values.empty()) return inferValueType(*command.values[0], locals, env);
        return {};
    }
    const std::string& op = command.instruction.name;
    if (op == "ref" && !command.values.empty()
        && command.values[0]->kind == ValueKind::Register) {
        TypeNode type = registerType(command.values[0]->registerValue, locals, env);
        ++type.pointerLevel;
        return {type, false, true};
    }
    if (op == "load") return {command.declaredType, false, command.hasDeclaredType};
    if (op == "call" && !command.values.empty()
        && command.values[0]->kind == ValueKind::Register) {
        auto result = env.returns.find(command.values[0]->registerValue.name);
        if (result != env.returns.end()) return {result->second, false, true};
    }
    if (!command.values.empty()) return inferValueType(*command.values[0], locals, env);
    if (command.hasDeclaredType) return {command.declaredType, false, true};
    return {};
}

bool isIntegerType(const TypeNode& type)
{
    return type.pointerLevel == 0 && (type.baseName == "bool" || type.baseName == "i8"
        || type.baseName == "i16" || type.baseName == "i32" || type.baseName == "i64"
        || type.baseName == "u8" || type.baseName == "u16" || type.baseName == "u32"
        || type.baseName == "u64");
}

bool isFloatingType(const TypeNode& type)
{
    return type.pointerLevel == 0 && (type.baseName == "f32" || type.baseName == "f64");
}

bool isIntegerVectorType(const TypeNode& type)
{
    return type.pointerLevel == 0 && (type.baseName == "i8x16" || type.baseName == "i16x8"
        || type.baseName == "i32x4" || type.baseName == "i64x2" || type.baseName == "i32x8"
        || type.baseName == "i64x4");
}

void validatePrimaryOperandTypes(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env)
{
    if (!command.hasInstruction || command.instruction.isSpecial) return;
    const std::string& op = command.instruction.name;
    const bool numeric = op == "add" || op == "sub" || op == "mul" || op == "div"
        || op == "mod" || op == "eq" || op == "neq" || op == "gt" || op == "lt"
        || op == "gte" || op == "lte";
    const bool integerBinary = op == "and" || op == "or" || op == "xor"
        || op == "lsh" || op == "rsh" || op == "land" || op == "lor";

    if (numeric || integerBinary) {
        if (command.values.size() != 2) return;
        ValueType left = inferValueType(*command.values[0], locals, env);
        ValueType right = inferValueType(*command.values[1], locals, env);
        auto isNumeric = [](const ValueType& value) {
            return value.known && (isIntegerType(value.type) || isFloatingType(value.type));
        };
        auto isInteger = [](const ValueType& value) {
            return value.known && isIntegerType(value.type);
        };
        require(!left.known || (numeric ? isNumeric(left) : isInteger(left)),
            "'" + op + "' requires " + (numeric ? "scalar numeric" : "integer") + " operands");
        require(!right.known || (numeric ? isNumeric(right) : isInteger(right)),
            "'" + op + "' requires " + (numeric ? "scalar numeric" : "integer") + " operands");
        if (left.known && right.known && isNumeric(left) && isNumeric(right))
            require(isIntegerType(left.type) == isIntegerType(right.type),
                "'" + op + "' cannot mix integer and floating-point operands");
        return;
    }

    if ((op == "not" || op == "lnot") && !command.values.empty()) {
        ValueType value = inferValueType(*command.values[0], locals, env);
        require(!value.known || isIntegerType(value.type)
                || (op == "not" && isIntegerVectorType(value.type)),
            "'" + op + "' requires an integer operand");
    }
    if (op == "cjmp" && !command.values.empty()) {
        ValueType value = inferValueType(*command.values[0], locals, env);
        require(!value.known || isIntegerType(value.type),
            "'cjmp' needs an integer condition");
    }
}

void validatePointerCast(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env)
{
    std::string name = command.instruction.name;
    std::replace(name.begin(), name.end(), '.', '_');
    if (!command.instruction.isSpecial
        || (name != "ptrcast" && name != "ptrtoint" && name != "inttoptr")) return;
    require(command.hasDeclaredType, "'#" + name + "' requires an explicitly typed result");
    require(command.values.size() == 1, "'#" + name + "' requires exactly one operand");
    ValueType source = inferValueType(*command.values[0], locals, env);
    if (name == "ptrcast") {
        require(source.known && !source.isNull && source.type.pointerLevel > 0,
            "'#ptrcast' requires a pointer operand");
        require(command.declaredType.pointerLevel > 0,
            "'#ptrcast' requires a pointer result type");
    } else if (name == "ptrtoint") {
        require(source.known && !source.isNull && source.type.pointerLevel > 0,
            "'#ptrtoint' requires a pointer operand");
        require(isIntegerType(command.declaredType),
            "'#ptrtoint' requires an integer result type");
    } else {
        require(source.known && source.type.pointerLevel == 0 && isIntegerType(source.type),
            "'#inttoptr' requires an integer operand");
        require(command.declaredType.pointerLevel > 0,
            "'#inttoptr' requires a pointer result type");
    }
}

void checkPointerConversion(const ValueType& source, const TypeNode& target, const std::string& context)
{
    if (target.pointerLevel > 0) {
        require(source.isNull || (source.known && source.type.pointerLevel > 0),
            "pointer type mismatch in " + context + ": expected " + target.baseName
                + std::string(static_cast<std::size_t>(target.pointerLevel), '*'));
        if (!source.isNull)
            require(sameType(source.type, target),
                "pointer type mismatch in " + context + ": expected " + target.baseName
                    + std::string(static_cast<std::size_t>(target.pointerLevel), '*'));
    } else if (source.known && source.type.pointerLevel > 0) {
        throw std::runtime_error("pointer value requires an explicit pointer conversion in " + context);
    }
}

void validateValueForType(const ValueNode& value, const TypeNode& target,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    const std::string& context)
{
    ValueType source = inferValueType(value, locals, env);
    if (value.kind == ValueKind::Array) {
        require(target.pointerLevel > 0, "array initializer requires a pointer type");
        TypeNode element = target;
        --element.pointerLevel;
        for (const ValueNode* item : value.arrayValues)
            validateValueForType(*item, element, locals, env, context);
        return;
    }
    if (context == "global initializer" && target.pointerLevel > 0
        && value.kind == ValueKind::Number)
        throw std::runtime_error("numeric global initializer requires an integer or floating type");
    if ((context == "global initializer" || context == "constant initializer")
        && target.pointerLevel == 0 && value.kind == ValueKind::String)
        throw std::runtime_error("string initializer requires a pointer type");
    checkPointerConversion(source, target, context);
}

void validateBlock(const BlockNode& block, const TypeEnvironment& env,
    std::unordered_map<std::string, TypeNode> locals = {},
    const TypeNode* returnType = nullptr)
{
    std::unordered_set<std::string> labels;
    for (const auto& command : block.commands) {
        if (command.isEmpty) continue;
        if (command.hasDeclaredType) {
            validateType(command.declaredType);
            require(locals.emplace(command.targetRegister.name, command.declaredType).second,
                "duplicate local declaration '" + command.targetRegister.name + "'");
        }
        if (command.hasInstruction) {
            validateInstruction(command, labels);
            validatePrimaryOperandTypes(command, locals, env);
            validatePointerCast(command, locals, env);
            const std::string& op = command.instruction.name;
            if (command.instruction.isSpecial) {
                std::string special = op;
                std::replace(special.begin(), special.end(), '.', '_');
                const bool atomicRead = special == "atomicload";
                const bool atomicWrite = special == "atomicstore";
                const bool compareExchange = special == "cas";
                const bool atomicRmw = special.rfind("atomic", 0) == 0
                    && !atomicRead && !atomicWrite;
                if (atomicRead || atomicWrite || compareExchange || atomicRmw) {
                    const std::size_t expectedCount = atomicRead ? 1 : atomicWrite ? 2
                        : compareExchange ? 3 : 2;
                    require(command.values.size() == expectedCount,
                        "incorrect operand count for '#" + special + "'");
                    ValueType pointerValue = inferValueType(*command.values[0], locals, env);
                    require(pointerValue.isNull
                            || (pointerValue.known && pointerValue.type.pointerLevel > 0),
                        "'#" + special + "' requires a pointer operand");
                    TypeNode pointer;
                    if (!pointerValue.isNull) {
                        pointer = pointerValue.type;
                        --pointer.pointerLevel;
                    }
                    if (atomicRead) {
                        require(command.hasDeclaredType
                                && (pointerValue.isNull || sameType(pointer, command.declaredType)),
                            "'#atomicload' result type must match the pointer's pointee type");
                    } else if (atomicWrite || atomicRmw) {
                        ValueType value = inferValueType(*command.values[1], locals, env);
                        require(value.known && (pointerValue.isNull || sameType(value.type, pointer)),
                            "pointer type mismatch in '#" + special + "' value operand");
                    } else {
                        for (std::size_t index = 1; index < 3; ++index) {
                            ValueType value = inferValueType(*command.values[index], locals, env);
                            require(value.known && (pointerValue.isNull || sameType(value.type, pointer)),
                                "pointer type mismatch in '#cas' comparison value");
                        }
                    }
                }
            }
            if (op == "ret" && returnType && !command.values.empty())
                validateValueForType(*command.values[0], *returnType, locals, env, "return");
            if (op == "store" && command.values.size() == 2) {
                ValueType pointerValue = inferValueType(*command.values[0], locals, env);
                require(pointerValue.isNull
                        || (pointerValue.known && pointerValue.type.pointerLevel > 0),
                    "'store' requires a pointer operand");
                ValueType stored = inferValueType(*command.values[1], locals, env);
                TypeNode pointee;
                if (!pointerValue.isNull) {
                    pointee = pointerValue.type;
                    --pointee.pointerLevel;
                }
                require(stored.known && (pointerValue.isNull
                            ? !stored.isNull
                            : (stored.isNull ? pointee.pointerLevel > 0
                                             : sameType(stored.type, pointee))),
                    "pointer type mismatch in store: value must match the pointer's pointee type");
            }
            if (op == "load" && !command.values.empty()) {
                ValueType pointerValue = inferValueType(*command.values[0], locals, env);
                require(pointerValue.isNull
                        || (pointerValue.known && pointerValue.type.pointerLevel > 0),
                    "'load' requires a pointer operand");
                TypeNode pointee;
                if (!pointerValue.isNull) {
                    pointee = pointerValue.type;
                    --pointee.pointerLevel;
                }
                require(pointerValue.isNull || sameType(pointee, command.declaredType),
                    "'load' result type must match the pointer's pointee type");
            }
            if (op == "call" && !command.values.empty()
                && command.values[0]->kind == ValueKind::Register) {
                const std::string& callee = command.values[0]->registerValue.name;
                auto parameters = env.parameters.find(callee);
                if (parameters != env.parameters.end()) {
                    require(command.values.size() - 1 == parameters->second.size(),
                        "incorrect argument count in call to '" + callee + "'");
                    for (std::size_t index = 0; index < parameters->second.size(); ++index)
                        validateValueForType(*command.values[index + 1], parameters->second[index],
                            locals, env, "argument to '" + callee + "'");
                }
            }
            if (command.targetKind == TargetKind::Register && !command.values.empty()) {
                TypeNode target = command.hasDeclaredType
                    ? command.declaredType : registerType(command.targetRegister, locals, env);
                const bool explicitPointerCast = command.instruction.isSpecial
                    && (op == "ptrcast" || op == "inttoptr");
                if (!explicitPointerCast) {
                    ValueType result = inferCommandType(command, locals, env);
                    if (result.known)
                        checkPointerConversion(result, target,
                            "result assigned to '" + command.targetRegister.name + "'");
                }
            }
        } else {
            require(command.hasDeclaredType || !command.values.empty(), "assignment requires a value");
            if (!command.values.empty() && command.targetKind == TargetKind::Register) {
                TypeNode target = command.hasDeclaredType
                    ? command.declaredType : registerType(command.targetRegister, locals, env);
                validateValueForType(*command.values[0], target, locals, env,
                    "assignment to '" + command.targetRegister.name + "'");
            }
        }
    }
}

void visitStructByValue(const std::string& name,
    const std::unordered_map<std::string, const StructDeclNode*>& structs,
    std::unordered_set<std::string>& visiting,
    std::unordered_set<std::string>& visited)
{
    auto structure = structs.find(name);
    if (visited.count(name) || structure == structs.end()) return;
    require(visiting.insert(name).second, "recursive by-value struct '" + name + "'");

    for (const auto& field : structure->second->fields) {
        if (field.type.pointerLevel == 0)
            visitStructByValue(field.type.baseName, structs, visiting, visited);
    }

    visiting.erase(name);
    visited.insert(name);
}

void validateStructCycles(const std::unordered_map<std::string, const StructDeclNode*>& structs)
{
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    for (const auto& structure : structs)
        visitStructByValue(structure.first, structs, visiting, visited);
}
}

void validateProgram(const ProgramNode& program)
{
    std::unordered_set<std::string> symbols;
    std::unordered_map<std::string, const StructDeclNode*> structs;
    TypeEnvironment env;
    for (const auto& decl : program.declarations) {
        switch (decl.kind) {
        case DeclKind::Struct:
            env.structs.emplace(decl.structDecl.type.baseName, &decl.structDecl);
            break;
        case DeclKind::Func: {
            const auto& fn = decl.funcDecl;
            env.returns.emplace(fn.name, fn.returnType);
            std::vector<TypeNode> parameters;
            for (const auto& arg : fn.args) parameters.push_back(arg.type);
            env.parameters.emplace(fn.name, std::move(parameters));
            break;
        }
        case DeclKind::Glob: env.globals.emplace(decl.globDecl.name, decl.globDecl.type); break;
        case DeclKind::Const: env.globals.emplace(decl.constDecl.name, decl.constDecl.type); break;
        case DeclKind::Entry: break;
        }
    }
    for (const auto& decl : program.declarations) {
        std::string name;
        switch (decl.kind) {
        case DeclKind::Entry:
            name = decl.entryDecl.name;
            break;
        case DeclKind::Func: {
            const auto& fn = decl.funcDecl;
            name = fn.name;
            validateType(fn.returnType, true);
            std::unordered_map<std::string, TypeNode> args;
            for (const auto& arg : fn.args) {
                validateType(arg.type);
                require(args.emplace(arg.name, arg.type).second, "duplicate argument '" + arg.name + "'");
            }
            break;
        }
        case DeclKind::Struct: {
            const auto& st = decl.structDecl;
            name = st.type.baseName;
            require(!contains(builtinTypes, name), "struct cannot redefine builtin type '" + name + "'");
            require(st.type.pointerLevel == 0, "struct name cannot be a pointer type");
            structs[name] = &st;
            std::unordered_set<std::string> fields;
            for (const auto& field : st.fields) {
                validateType(field.type);
                require(fields.insert(field.name).second, "duplicate field '" + field.name + "'");
            }
            break;
        }
        case DeclKind::Glob:
            name = decl.globDecl.name;
            validateType(decl.globDecl.type);
            if (decl.globDecl.value)
                validateValueForType(*decl.globDecl.value, decl.globDecl.type, {}, env, "global initializer");
            break;
        case DeclKind::Const:
            name = decl.constDecl.name;
            validateType(decl.constDecl.type);
            if (decl.constDecl.value)
                validateValueForType(*decl.constDecl.value, decl.constDecl.type, {}, env, "constant initializer");
            break;
        }
        require(symbols.insert(name).second, "duplicate declaration '" + name + "'");
    }
    validateStructCycles(structs);
    for (const auto& decl : program.declarations) {
        if (decl.kind == DeclKind::Entry) {
            validateBlock(decl.entryDecl.body, env);
        } else if (decl.kind == DeclKind::Func && decl.funcDecl.hasBody) {
            std::unordered_map<std::string, TypeNode> args;
            for (const auto& arg : decl.funcDecl.args) args.emplace(arg.name, arg.type);
            validateBlock(decl.funcDecl.body, env, std::move(args), &decl.funcDecl.returnType);
        }
    }
}
