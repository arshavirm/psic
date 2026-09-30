#include "validation.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
thread_local SourceLocation activeLocation;

constexpr std::array<std::string_view, 18> binaryInstructions = {
    "add", "sub", "mul", "div", "mod", "eq", "gt", "lt", "neq", "gte", "lte",
    "and", "or", "xor", "lsh", "rsh", "land", "lor"};
constexpr std::array<std::string_view, 6> unaryInstructions = {
    "not", "lnot", "load", "ref", "jmp", "label"};
constexpr std::array<std::string_view, 15> atomicInstructions = {
    "atomicload", "atomicstore", "atomicadd", "atomicsub", "atomicand",
    "atomicor", "atomicxor", "atomicnand", "atomicxchg", "atomicmax",
    "atomicmin", "atomicumax", "atomicumin", "atomicfadd", "atomicfsub"};

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

std::string normalizedInstructionName(const std::string& name)
{
    std::string normalized = name;
    std::replace(normalized.begin(), normalized.end(), '.', '_');
    return normalized;
}

std::optional<std::string_view> pointerDifferenceResultType(unsigned pointerBits)
{
    switch (pointerBits) {
    case 8: return "i8";
    case 16: return "i16";
    case 32: return "i32";
    case 64: return "i64";
    default: return std::nullopt;
    }
}

void validateType(const TypeNode& type, bool allowVoid = false)
{
    require(!type.isView || type.pointerLevel == 0,
        "a view type cannot be wrapped in a pointer");
    require(!type.isView || type.baseName != "void",
        "a bounded view requires a value element type");
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
    std::unordered_set<std::string> constantObjects;
    std::unordered_set<std::string> immutableGlobalBacking;
    std::unordered_map<std::string, TypeNode> returns;
    std::unordered_map<std::string, std::vector<TypeNode>> parameters;
    std::unordered_map<std::string, const StructDeclNode*> structs;
};

bool sameType(const TypeNode& left, const TypeNode& right)
{
    return left.baseName == right.baseName && left.pointerLevel == right.pointerLevel
        && left.isView == right.isView;
}

bool compatiblePointerTypes(const TypeNode& left, const TypeNode& right)
{
    if (sameType(left, right)) return true;

    // Object pointers may pass through void*, but pointer-to-pointer conversions
    // do not inherit that rule (for example, i32** is not interchangeable with
    // void**).
    return left.pointerLevel == 1 && right.pointerLevel == 1
        && (left.baseName == "void" || right.baseName == "void");
}

bool mayNameImmutableBacking(const ValueNode& value,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    const std::unordered_set<std::string>& immutableAliases)
{
    if (value.kind == ValueKind::String) return true;
    if (value.kind != ValueKind::Register) return false;
    if (immutableAliases.find(value.registerValue.name) != immutableAliases.end())
        return true;
    if (!value.registerValue.accessors.empty()) return false;
    if (locals.find(value.registerValue.name) != locals.end())
        return false;
    return env.immutableGlobalBacking.find(value.registerValue.name)
        != env.immutableGlobalBacking.end();
}

bool commandMayNameImmutableBacking(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    const std::unordered_set<std::string>& immutableAliases)
{
    if (command.values.empty()) return false;
    if (!command.hasInstruction)
        return mayNameImmutableBacking(*command.values[0], locals, env, immutableAliases);

    const std::string instructionName = normalizedInstructionName(command.instruction.name);
    if (instructionName != "ptrcast" && instructionName != "ptrtoint"
        && instructionName != "inttoptr" && instructionName != "add"
        && instructionName != "ref")
        return false;
    return std::any_of(command.values.begin(), command.values.end(),
        [&](const ValueNode* value) {
            return mayNameImmutableBacking(*value, locals, env, immutableAliases);
        });
}

bool mayPointIntoLocalStorage(const ValueNode& value,
    const std::unordered_map<std::string, TypeNode>& locals,
    const std::unordered_set<std::string>& localStorageAliases)
{
    return value.kind == ValueKind::Register
        && value.registerValue.accessors.empty()
        && locals.find(value.registerValue.name) != locals.end()
        && localStorageAliases.find(value.registerValue.name) != localStorageAliases.end();
}

struct TypeEnvironment;
bool mayContainLocalPointer(const ValueNode& value,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    const std::unordered_set<std::string>& localStorageAliases,
    const std::unordered_set<std::string>& localAggregateContents);
bool isIntegerType(const TypeNode& type);
bool isFloatingType(const TypeNode& type);
bool isPointerDifferenceCommand(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env);

bool accessorTargetIsLocalStorage(const RegNode& target,
    const std::unordered_map<std::string, TypeNode>& locals,
    const std::unordered_set<std::string>& localStorageAliases)
{
    const auto root = locals.find(target.name);
    if (root == locals.end()) return false;

    std::size_t indexCount = 0;
    for (const AccessorNode& accessor : target.accessors)
        if (accessor.kind == AccessorKind::Index) ++indexCount;
    if (indexCount == 0) return true; // Local aggregate fields stay in the local slot.

    // A single leading index addresses an element in a pointer known to refer
    // to local storage. Later indexes may follow a pointer loaded from that
    // element, whose destination region is not known to this analysis.
    return target.accessors.front().kind == AccessorKind::Index
        && indexCount == 1
        && root->second.pointerLevel > 0
        && localStorageAliases.find(target.name) != localStorageAliases.end();
}

bool commandMayPointIntoLocalStorage(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals,
    const TypeEnvironment& env,
    const std::unordered_set<std::string>& localStorageAliases,
    const std::unordered_map<std::string, std::string>& localReferences,
    const std::unordered_set<std::string>& localAggregateContents)
{
    if (command.values.empty()) return false;
    if (!command.hasInstruction)
        return mayContainLocalPointer(*command.values[0], locals, env, localStorageAliases,
                   localAggregateContents)
            || command.values[0]->kind == ValueKind::Array;

    const std::string instructionName = normalizedInstructionName(command.instruction.name);
    if (instructionName == "ref" && command.values[0]->kind == ValueKind::Register) {
        const RegNode& referenced = command.values[0]->registerValue;
        const bool isLocal = locals.find(referenced.name) != locals.end();
        const bool hasIndex = std::any_of(referenced.accessors.begin(), referenced.accessors.end(),
            [](const AccessorNode& accessor) { return accessor.kind == AccessorKind::Index; });
        return isLocal && (!hasIndex
            || localStorageAliases.find(referenced.name) != localStorageAliases.end());
    }
    if (instructionName == "load" || instructionName == "atomicload") {
        if (!command.hasDeclaredType) return false;
        const bool pointerResult = command.declaredType.pointerLevel > 0;
        const auto* source = command.values[0];
        const bool scalarFromTaintedAggregate =
            (isIntegerType(command.declaredType) || isFloatingType(command.declaredType))
            && source->kind == ValueKind::Register
            && !source->registerValue.accessors.empty()
            && localAggregateContents.find(source->registerValue.name)
                != localAggregateContents.end();
        if ((pointerResult || scalarFromTaintedAggregate)
            && mayContainLocalPointer(*source, locals, env, localStorageAliases,
                localAggregateContents))
            return true;
        if (source->kind != ValueKind::Register) return false;
        const auto reference = localReferences.find(source->registerValue.name);
        return reference != localReferences.end()
            && localStorageAliases.find(reference->second) != localStorageAliases.end()
            && (pointerResult || isIntegerType(command.declaredType)
                || isFloatingType(command.declaredType));
    }
    if (instructionName == "sub" && isPointerDifferenceCommand(command, locals, env))
        return false; // Pointer differences are integers, not address values.
    if (instructionName == "ptrcast" || instructionName == "ptrtoint"
        || instructionName == "inttoptr" || instructionName == "trunc"
        || instructionName == "sext" || instructionName == "zext"
        || instructionName == "sitofp" || instructionName == "uitofp"
        || instructionName == "fptosi" || instructionName == "fptoui"
        || instructionName == "fpext" || instructionName == "fptrunc"
        || instructionName == "add" || instructionName == "sub"
        || instructionName == "mul" || instructionName == "div"
        || instructionName == "mod" || instructionName == "and"
        || instructionName == "or" || instructionName == "xor"
        || instructionName == "lsh" || instructionName == "rsh"
        || instructionName == "not" || instructionName == "bswap"
        || instructionName == "bitreverse") {
        return std::any_of(command.values.begin(), command.values.end(),
            [&](const ValueNode* value) {
                return mayContainLocalPointer(*value, locals, env, localStorageAliases,
                    localAggregateContents);
            });
    }
    return false;
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
        require(!type.isView, "a bounded view must be indexed with #viewload or #viewstore");
        if (accessor.kind == AccessorKind::Index) {
            require(type.pointerLevel > 0, "index accessor requires a pointer");
            require(type.pointerLevel > 1 || type.baseName != "void",
                "cannot index void*; convert it to an object pointer first");
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

bool mayContainLocalPointer(const ValueNode& value,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    const std::unordered_set<std::string>& localStorageAliases,
    const std::unordered_set<std::string>& localAggregateContents)
{
    if (mayPointIntoLocalStorage(value, locals, localStorageAliases)) return true;
    if (value.kind != ValueKind::Register || value.registerValue.accessors.empty())
        return false;

    const auto root = locals.find(value.registerValue.name);
    if (root == locals.end()
        || localStorageAliases.find(value.registerValue.name) == localStorageAliases.end())
        return false;

    // Pointer reads from local backing and address-sized integer reads from a
    // tainted aggregate may recover a local-storage alias.
    const TypeNode accessedType = registerType(value.registerValue, locals, env);
    if (accessedType.pointerLevel > 0) return true;
    if (localAggregateContents.find(value.registerValue.name)
            != localAggregateContents.end()) {
        return isIntegerType(accessedType)
            || isFloatingType(accessedType)
            || env.structs.find(accessedType.baseName) != env.structs.end();
    }
    return false;
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
    case ValueKind::Number: return {{value.numberIsFloat ? "f64" : "i64", 0}, false, true};
    case ValueKind::Bool: return {{"bool", 0}, false, true};
    case ValueKind::String: return {{"i8", 1}, false, true};
    case ValueKind::Null: return {{}, true, true};
    case ValueKind::Register: return {registerType(value.registerValue, locals, env), false, true};
    case ValueKind::Array:
    case ValueKind::SpecialRegister: return {};
    }
    return {};
}

bool isPointerDifferenceCommand(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env)
{
    if (command.values.size() != 2) return false;
    const ValueType left = inferValueType(*command.values[0], locals, env);
    const ValueType right = inferValueType(*command.values[1], locals, env);
    return left.known && right.known
        && left.type.pointerLevel > 0 && right.type.pointerLevel > 0;
}

ValueType inferCommandType(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    PointerModel pointerModel)
{
    if (command.instruction.isSpecial) {
        const std::string name = normalizedInstructionName(command.instruction.name);
        if (name == "ptrcast" || name == "inttoptr" || name == "atomicload" || name == "vsplat"
            || name == "viewlen" || name == "view_len"
            || name == "viewload" || name == "view_load"
            || name == "trunc" || name == "sext" || name == "zext"
            || name == "fpext" || name == "fptrunc" || name == "sitofp"
            || name == "uitofp" || name == "fptosi" || name == "fptoui")
            return {command.declaredType, false, command.hasDeclaredType};
        if (name == "ptrtoint") return {command.declaredType, false, command.hasDeclaredType};
        if (name == "viewlen" || name == "view_len")
            return {{"u64", 0}, false, command.hasDeclaredType};
        if (name == "viewload" || name == "view_load")
            return {command.declaredType, false, command.hasDeclaredType};
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
    if ((op == "add" || op == "sub") && command.values.size() == 2) {
        ValueType left = inferValueType(*command.values[0], locals, env);
        ValueType right = inferValueType(*command.values[1], locals, env);
        const bool leftPointer = left.known && left.type.pointerLevel > 0;
        const bool rightPointer = right.known && right.type.pointerLevel > 0;
        if (op == "add" && leftPointer != rightPointer)
            return {leftPointer ? left.type : right.type, false, true};
        if (op == "sub" && leftPointer && rightPointer) {
            const auto resultType = pointerDifferenceResultType(pointerModel.bits);
            if (resultType) return {{std::string(*resultType), 0}, false, true};
            return {};
        }
    }
    if ((op == "eq" || op == "neq" || op == "gt" || op == "lt" || op == "gte" || op == "lte")
        && command.values.size() == 2) {
        return {{"bool", 0}, false, true};
    }
    if (op == "call" && !command.values.empty()
        && command.values[0]->kind == ValueKind::Register) {
        auto result = env.returns.find(command.values[0]->registerValue.name);
        if (result != env.returns.end()) return {result->second, false, true};
        if (command.hasDeclaredType) return {command.declaredType, false, true};
    }
    if (!command.values.empty()) return inferValueType(*command.values[0], locals, env);
    if (command.hasDeclaredType) return {command.declaredType, false, true};
    return {};
}

bool isIntegerType(const TypeNode& type)
{
    return !type.isView && type.pointerLevel == 0
        && (type.baseName == "bool" || type.baseName == "i8"
        || type.baseName == "i16" || type.baseName == "i32" || type.baseName == "i64"
        || type.baseName == "u8" || type.baseName == "u16" || type.baseName == "u32"
        || type.baseName == "u64");
}

bool isSignedIntegerType(const TypeNode& type)
{
    return !type.isView && type.pointerLevel == 0
        && (type.baseName == "i8" || type.baseName == "i16"
        || type.baseName == "i32" || type.baseName == "i64");
}

bool isUnsignedIntegerType(const TypeNode& type)
{
    return !type.isView && type.pointerLevel == 0
        && (type.baseName == "u8" || type.baseName == "u16"
        || type.baseName == "u32" || type.baseName == "u64");
}

unsigned integerWidth(const TypeNode& type)
{
    if (type.baseName == "bool") return 1;
    return static_cast<unsigned>(std::stoul(type.baseName.substr(1)));
}

bool isFloatingType(const TypeNode& type)
{
    return !type.isView && type.pointerLevel == 0
        && (type.baseName == "f32" || type.baseName == "f64");
}

bool isAtomicIntegerType(const TypeNode& type)
{
    return !type.isView && type.pointerLevel == 0
        && (type.baseName == "i8" || type.baseName == "i16"
        || type.baseName == "i32" || type.baseName == "i64" || type.baseName == "u8"
        || type.baseName == "u16" || type.baseName == "u32" || type.baseName == "u64");
}

bool isAtomicLoadStoreType(const TypeNode& type)
{
    return type.pointerLevel > 0 || isAtomicIntegerType(type) || isFloatingType(type);
}

bool isIntegerVectorType(const TypeNode& type)
{
    return !type.isView && type.pointerLevel == 0
        && (type.baseName == "i8x16" || type.baseName == "i16x8"
        || type.baseName == "i32x4" || type.baseName == "i64x2" || type.baseName == "i32x8"
        || type.baseName == "i64x4");
}

void validatePrimaryOperandTypes(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    PointerModel pointerModel)
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
        const bool pointerOp = op == "add" || op == "sub" || op == "eq" || op == "neq"
            || op == "gt" || op == "lt" || op == "gte" || op == "lte";
        const bool leftPointer = left.known && left.type.pointerLevel > 0;
        const bool rightPointer = right.known && right.type.pointerLevel > 0;
        const bool allowsNullEquality = (op == "eq" || op == "neq") && left.isNull;
        const bool allowsNullRight = (op == "eq" || op == "neq") && right.isNull;
        require(!left.known || (pointerOp && leftPointer) || allowsNullEquality || (numeric ? isNumeric(left) : isInteger(left)),
            "'" + op + "' requires " + (numeric ? "scalar numeric" : "integer") + " operands");
        require(!right.known || (pointerOp && rightPointer) || allowsNullRight || (numeric ? isNumeric(right) : isInteger(right)),
            "'" + op + "' requires " + (numeric ? "scalar numeric" : "integer") + " operands");
        if (leftPointer || rightPointer) {
            require(pointerOp, "pointer operands are not valid for '" + op + "'");
            if (op == "add") {
                require(pointerModel.integral,
                    "pointer addition is unavailable for a non-integral pointer target");
                require(leftPointer != rightPointer, "pointer addition requires exactly one pointer operand");
                const TypeNode& pointer = leftPointer ? left.type : right.type;
                const ValueType& offset = leftPointer ? right : left;
                require(offset.known && isIntegerType(offset.type), "pointer arithmetic requires an integer offset");
                require(pointer.pointerLevel != 1 || pointer.baseName != "void",
                    "pointer arithmetic requires a complete object type");
            } else if (op == "sub") {
                require(leftPointer && rightPointer && sameType(left.type, right.type),
                    "pointer subtraction requires a pointer left operand and matching pointer types");
                require(pointerDifferenceResultType(pointerModel.bits).has_value(),
                    "pointer subtraction requires a target pointer width with a matching signed PSI integer type");
                require(pointerModel.integral,
                    "pointer subtraction is unavailable for a non-integral pointer target");
                require(left.type.pointerLevel != 1 || left.type.baseName != "void",
                    "pointer arithmetic requires a complete object type");
            } else if (op == "eq" || op == "neq") {
                require((leftPointer && (rightPointer
                            ? compatiblePointerTypes(left.type, right.type) : right.isNull))
                        || (rightPointer && left.isNull),
                    "pointer equality requires matching pointer types or a null pointer");
            } else {
                require(leftPointer && rightPointer && sameType(left.type, right.type),
                    "pointer comparison requires matching pointer types");
                require(pointerModel.integral,
                    "ordered pointer comparisons are unavailable for a non-integral pointer target");
            }
        }
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

void validateExplicitCast(const CommandNode& command,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    PointerModel pointerModel)
{
    const std::string name = normalizedInstructionName(command.instruction.name);
    constexpr std::array<std::string_view, 12> casts = {
        "ptrcast", "ptrtoint", "inttoptr", "trunc", "sext", "zext", "fpext", "fptrunc",
        "sitofp", "uitofp", "fptosi", "fptoui"};
    if (!command.instruction.isSpecial || !contains(casts, name)) return;
    require(command.hasDeclaredType, "'#" + name + "' requires an explicitly typed result");
    require(command.values.size() == 1, "'#" + name + "' requires exactly one operand");
    ValueType source = inferValueType(*command.values[0], locals, env);
    if (name == "ptrcast") {
        require(source.isNull || (source.known && source.type.pointerLevel > 0),
            "'#ptrcast' requires a pointer operand");
        require(command.declaredType.pointerLevel > 0,
            "'#ptrcast' requires a pointer result type");
    } else if (name == "ptrtoint") {
        require(source.isNull || (source.known && source.type.pointerLevel > 0),
            "'#ptrtoint' requires a pointer operand");
        require(isIntegerType(command.declaredType),
            "'#ptrtoint' requires an integer result type");
        require(pointerModel.integral,
            "'#ptrtoint' is unavailable for a non-integral pointer target");
        if (isIntegerType(command.declaredType))
            require(integerWidth(command.declaredType) == pointerModel.bits,
                "'#ptrtoint' result width must match the target pointer width");
    } else if (name == "inttoptr") {
        require(source.known && source.type.pointerLevel == 0 && isIntegerType(source.type),
            "'#inttoptr' requires an integer operand");
        require(command.declaredType.pointerLevel > 0,
            "'#inttoptr' requires a pointer result type");
        require(pointerModel.integral,
            "'#inttoptr' is unavailable for a non-integral pointer target");
        if (source.known && isIntegerType(source.type))
            require(integerWidth(source.type) == pointerModel.bits,
                "'#inttoptr' operand width must match the target pointer width");
    } else if (name == "sitofp" || name == "uitofp") {
        require(source.known && (name == "sitofp"
                    ? isSignedIntegerType(source.type) : isIntegerType(source.type))
                && command.declaredType.pointerLevel == 0
                && isFloatingType(command.declaredType),
            name == "sitofp"
                ? "'#sitofp' requires a signed integer source and floating-point result"
                : "'#uitofp' requires an integer source and floating-point result");
    } else if (name == "fptosi" || name == "fptoui") {
        require(source.known && isFloatingType(source.type)
                && command.declaredType.pointerLevel == 0
                && (name == "fptosi" ? isSignedIntegerType(command.declaredType)
                                      : isUnsignedIntegerType(command.declaredType)),
            name == "fptosi"
                ? "'#fptosi' requires a floating-point source and signed integer result"
                : "'#fptoui' requires a floating-point source and unsigned integer result");
    } else {
        require(source.known && source.type.pointerLevel == 0
                && command.declaredType.pointerLevel == 0,
            "'" + std::string("#") + name + "' requires scalar source and result types");
        if (name == "trunc" || name == "sext" || name == "zext") {
            require(isIntegerType(source.type) && isIntegerType(command.declaredType),
                "'" + std::string("#") + name + "' requires integer types");
            if (isIntegerType(source.type) && isIntegerType(command.declaredType)) {
                const unsigned sourceWidth = integerWidth(source.type);
                const unsigned resultWidth = integerWidth(command.declaredType);
                require(name == "trunc" ? sourceWidth > resultWidth : sourceWidth < resultWidth,
                    "'" + std::string("#") + name + "' requires "
                        + (name == "trunc" ? "a narrower result" : "a wider result"));
            }
        } else {
            require(isFloatingType(source.type) && isFloatingType(command.declaredType),
                "'#" + name + "' requires floating-point types");
            const unsigned sourceWidth = source.type.baseName == "f32" ? 32 : 64;
            const unsigned resultWidth = command.declaredType.baseName == "f32" ? 32 : 64;
            require(name == "fpext" ? sourceWidth < resultWidth : sourceWidth > resultWidth,
                "'#" + name + "' requires "
                    + (name == "fpext" ? "a wider result" : "a narrower result"));
        }
    }
}

void checkPointerConversion(const ValueType& source, const TypeNode& target, const std::string& context)
{
    if (target.pointerLevel > 0) {
        require(source.isNull || (source.known && source.type.pointerLevel > 0),
            "pointer type mismatch in " + context + ": expected " + target.baseName
                + std::string(static_cast<std::size_t>(target.pointerLevel), '*'));
        if (!source.isNull)
            require(compatiblePointerTypes(source.type, target),
                "pointer type mismatch in " + context + ": expected " + target.baseName
                    + std::string(static_cast<std::size_t>(target.pointerLevel), '*'));
    } else if (source.known && source.type.pointerLevel > 0) {
        throw std::runtime_error("pointer value requires an explicit pointer conversion in " + context);
    } else if (source.isNull) {
        throw std::runtime_error("null pointer value requires a pointer destination in " + context);
    }
}

void validateValueForType(const ValueNode& value, const TypeNode& target,
    const std::unordered_map<std::string, TypeNode>& locals, const TypeEnvironment& env,
    const std::string& context)
{
    ValueType source = inferValueType(value, locals, env);
    if (value.kind == ValueKind::Array) {
        require(target.pointerLevel > 0 || target.isView,
            "array initializer requires a pointer or bounded view type");
        TypeNode element = target;
        if (target.isView) element.isView = false;
        else --element.pointerLevel;
        for (const ValueNode* item : value.arrayValues)
            validateValueForType(*item, element, locals, env, context);
        return;
    }
    if (target.isView || source.type.isView)
        require(target.isView && source.known && sameType(source.type, target),
            "bounded views can only be initialized or assigned from the same view element type");
    if (value.kind == ValueKind::Number) {
        require(target.pointerLevel == 0 && !target.isView,
            "numeric literal requires a scalar numeric destination");
        require(value.numberIsFloat ? isFloatingType(target) : isIntegerType(target),
            value.numberIsFloat
                ? "floating-point literal requires a floating-point destination"
                : "integer literal requires an integer destination");
    }
    if ((context == "global initializer" || context == "constant initializer")
        && target.pointerLevel == 0 && value.kind == ValueKind::String)
        throw std::runtime_error("string initializer requires a pointer type");
    checkPointerConversion(source, target, context);
}

void validateBlock(const BlockNode& block, const TypeEnvironment& env,
    std::unordered_map<std::string, TypeNode> locals = {},
    const TypeNode* returnType = nullptr, PointerModel pointerModel = {})
{
    std::unordered_set<std::string> labels;
    // Conservatively retain a local's immutable-storage provenance once seen.
    // PSI control flow can jump between source locations, so removing a taint
    // after one apparent reassignment would make source-order analysis unsound.
    std::unordered_set<std::string> immutableAliases;
    std::unordered_set<std::string> localStorageAliases;
    std::unordered_set<std::string> localAggregateContents;
    std::unordered_set<std::string> localAggregateReferences;
    std::vector<std::unordered_map<std::string, TypeNode>> visibleLocals;
    visibleLocals.reserve(block.commands.size());
    auto localsAfterCommand = locals;
    for (const auto& command : block.commands) {
        visibleLocals.push_back(localsAfterCommand);
        if (command.hasDeclaredType)
            localsAfterCommand.emplace(command.targetRegister.name, command.declaredType);
    }
    const auto initializerContainsLocalAddress = [&](auto&& self, const ValueNode& value,
        const std::unordered_map<std::string, TypeNode>& scope) -> bool {
        if (value.kind == ValueKind::Array) {
            return std::any_of(value.arrayValues.begin(), value.arrayValues.end(),
                [&](const ValueNode* element) {
                    return self(self, *element, scope);
                });
        }
        return mayContainLocalPointer(value, scope, env, localStorageAliases,
            localAggregateContents);
    };

    // Compute a conservative function-wide closure so a jump cannot hide a
    // later local-storage alias from an earlier return or call site.
    bool addedLocalAlias;
    std::unordered_map<std::string, std::string> localReferences;
    do {
        addedLocalAlias = false;
        for (std::size_t index = 0; index < block.commands.size(); ++index) {
            const CommandNode& command = block.commands[index];
            if (command.hasInstruction && command.values.size() == 2) {
                const std::string instructionName = normalizedInstructionName(
                    command.instruction.name);
                const bool storesThroughPointer = (!command.instruction.isSpecial
                        && instructionName == "store")
                    || (command.instruction.isSpecial && instructionName == "atomicstore");
                const ValueNode* destination = command.values[0];
                if (storesThroughPointer && destination->kind == ValueKind::Register
                    && destination->registerValue.accessors.empty()
                    && localStorageAliases.find(destination->registerValue.name)
                        != localStorageAliases.end()) {
                    const auto reference = localReferences.find(
                        destination->registerValue.name);
                    if (reference != localReferences.end()
                        && immutableAliases.find(reference->second) == immutableAliases.end()
                        && mayNameImmutableBacking(*command.values[1],
                            visibleLocals[index], env, immutableAliases)
                        && immutableAliases.insert(reference->second).second)
                        addedLocalAlias = true;
                    if (reference != localReferences.end()
                        && mayContainLocalPointer(*command.values[1], visibleLocals[index],
                            env, localStorageAliases, localAggregateContents)) {
                        addedLocalAlias |= localStorageAliases.insert(reference->second).second;
                        if (localAggregateReferences.find(destination->registerValue.name)
                                != localAggregateReferences.end())
                            addedLocalAlias |= localAggregateContents.insert(reference->second).second;
                    }
                }
            }
            if (command.hasInstruction && command.instruction.isSpecial
                && normalizedInstructionName(command.instruction.name) == "memcpy"
                && command.values.size() == 3
                && command.values[0]->kind == ValueKind::Register
                && command.values[1]->kind == ValueKind::Register) {
                const std::string& destinationName = command.values[0]->registerValue.name;
                const std::string& sourceName = command.values[1]->registerValue.name;
                const auto sourceReference = localReferences.find(sourceName);
                const bool sourceContainsAddress =
                    localAggregateContents.find(sourceName) != localAggregateContents.end()
                    || (sourceReference != localReferences.end()
                        && localAggregateContents.find(sourceReference->second)
                            != localAggregateContents.end());
                if (sourceContainsAddress
                    && mayContainLocalPointer(*command.values[0], visibleLocals[index],
                        env, localStorageAliases, localAggregateContents)) {
                    if (localAggregateContents.insert(destinationName).second)
                        addedLocalAlias = true;
                    const auto destinationReference = localReferences.find(destinationName);
                    if (destinationReference != localReferences.end()
                        && localAggregateContents.insert(destinationReference->second).second)
                        addedLocalAlias = true;
                }
            }
            if (command.targetKind == TargetKind::Register
                && command.targetRegister.accessors.empty()
                && command.hasInstruction && command.values.size() == 1) {
                const std::string instructionName = normalizedInstructionName(
                    command.instruction.name);
                const ValueNode* source = command.values[0];
                if ((instructionName == "load" || instructionName == "atomicload")
                    && source->kind == ValueKind::Register
                    && source->registerValue.accessors.empty()) {
                    const auto reference = localReferences.find(source->registerValue.name);
                    if (reference != localReferences.end()
                        && immutableAliases.find(reference->second) != immutableAliases.end()
                        && immutableAliases.insert(command.targetRegister.name).second)
                        addedLocalAlias = true;
                }
            }
            if (command.targetKind != TargetKind::Register) continue;
            if (!command.targetRegister.accessors.empty()) {
                const std::string& aggregateName = command.targetRegister.name;
                if (visibleLocals[index].find(aggregateName) != visibleLocals[index].end()
                    && registerType(command.targetRegister, visibleLocals[index], env).pointerLevel > 0
                    && immutableAliases.find(aggregateName) == immutableAliases.end()
                    && commandMayNameImmutableBacking(command, visibleLocals[index],
                        env, immutableAliases)
                    && immutableAliases.insert(aggregateName).second)
                    addedLocalAlias = true;
                if (accessorTargetIsLocalStorage(command.targetRegister,
                        visibleLocals[index], localStorageAliases)
                    && commandMayPointIntoLocalStorage(command, visibleLocals[index],
                        env, localStorageAliases, localReferences, localAggregateContents)) {
                    // A pointer-bearing local aggregate carries the lifetime
                    // of every tracked local address stored in one of its
                    // fields. Taint the aggregate so whole-value copies,
                    // returns, and calls cannot hide that escape.
                    addedLocalAlias |= localStorageAliases.insert(aggregateName).second;
                    addedLocalAlias |= localAggregateContents.insert(aggregateName).second;
                }
                continue;
            }
            const auto local = visibleLocals[index].find(command.targetRegister.name);
            const bool declaresLocal = command.hasDeclaredType;
            if (!declaresLocal && local == visibleLocals[index].end()) continue;
            const TypeNode& targetType = declaresLocal
                ? command.declaredType : local->second;
            const std::string instructionName = command.hasInstruction
                ? normalizedInstructionName(command.instruction.name) : std::string{};
            if (!command.values.empty() && command.values[0]->kind == ValueKind::Array
                && initializerContainsLocalAddress(initializerContainsLocalAddress,
                    *command.values[0], visibleLocals[index])
                && localAggregateContents.insert(command.targetRegister.name).second)
                addedLocalAlias = true;
            const bool copiesTaintedAggregate = std::any_of(
                command.values.begin(), command.values.end(), [&](const ValueNode* value) {
                    return value->kind == ValueKind::Register
                        && localAggregateContents.find(value->registerValue.name)
                            != localAggregateContents.end();
                });
            if (copiesTaintedAggregate
                && localAggregateContents.insert(command.targetRegister.name).second)
                addedLocalAlias = true;
            std::string referencedLocal;
            if (instructionName == "ref" && !command.values.empty()
                && command.values[0]->kind == ValueKind::Register
                && visibleLocals[index].find(
                    command.values[0]->registerValue.name) != visibleLocals[index].end()) {
                const RegNode& referenced = command.values[0]->registerValue;
                referencedLocal = referenced.name;
                const TypeNode& referencedRoot = visibleLocals[index].at(referenced.name);
                const bool referencesStructStorage = referencedRoot.pointerLevel == 0
                    && env.structs.find(referencedRoot.baseName) != env.structs.end();
                const bool referencesIndexedLocalBacking = referencedRoot.pointerLevel > 0
                    && std::any_of(referenced.accessors.begin(), referenced.accessors.end(),
                        [](const AccessorNode& accessor) {
                            return accessor.kind == AccessorKind::Index;
                        })
                    && localStorageAliases.find(referenced.name) != localStorageAliases.end();
                if ((referencesStructStorage || referencesIndexedLocalBacking)
                    && localAggregateReferences.insert(command.targetRegister.name).second)
                    addedLocalAlias = true;
            } else if (!command.values.empty()
                && (!command.hasInstruction || instructionName == "ptrcast"
                    || instructionName == "ptrtoint" || instructionName == "inttoptr"
                    || instructionName == "trunc" || instructionName == "sext"
                    || instructionName == "zext" || instructionName == "add")) {
                // Preserve the local slot relationship through direct copies,
                // representation-preserving casts, and address offsets. If
                // operands refer to different slots, retain no exact mapping.
                for (const ValueNode* value : command.values) {
                    if (value->kind != ValueKind::Register
                        || !value->registerValue.accessors.empty()) continue;
                    const auto reference = localReferences.find(value->registerValue.name);
                    if (reference == localReferences.end()) continue;
                    if (referencedLocal.empty()) referencedLocal = reference->second;
                    else if (referencedLocal != reference->second) {
                        referencedLocal.clear();
                        break;
                    }
                }
            }
            if (!referencedLocal.empty()) {
                const auto inserted = localReferences.emplace(
                    command.targetRegister.name, std::move(referencedLocal));
                if (inserted.second) addedLocalAlias = true;
            }
            const bool preservesAggregateReference = !command.hasInstruction
                || instructionName == "ptrcast" || instructionName == "ptrtoint"
                || instructionName == "inttoptr" || instructionName == "trunc"
                || instructionName == "sext" || instructionName == "zext"
                || instructionName == "add";
            if (preservesAggregateReference
                && std::any_of(command.values.begin(), command.values.end(),
                    [&](const ValueNode* value) {
                        return value->kind == ValueKind::Register
                            && localAggregateReferences.find(value->registerValue.name)
                                != localAggregateReferences.end();
                    })
                && localAggregateReferences.insert(command.targetRegister.name).second)
                addedLocalAlias = true;
            if (commandMayNameImmutableBacking(command, visibleLocals[index],
                    env, immutableAliases)
                && immutableAliases.insert(command.targetRegister.name).second)
                addedLocalAlias = true;
            const bool tracksAddressBits = targetType.pointerLevel > 0
                || isIntegerType(targetType) || isFloatingType(targetType);
            if (!tracksAddressBits && instructionName != "ptrtoint") continue;
            if (localStorageAliases.find(command.targetRegister.name)
                    == localStorageAliases.end()
                && commandMayPointIntoLocalStorage(command, visibleLocals[index],
                    env, localStorageAliases, localReferences, localAggregateContents)) {
                localStorageAliases.insert(command.targetRegister.name);
                addedLocalAlias = true;
            }
        }
    } while (addedLocalAlias);

    for (const auto& command : block.commands) {
        activeLocation = command.location;
        if (command.isEmpty) continue;
        // Indexed pointer accesses need address-bit checks in codegen.
        auto hasPointerIndex = [](const RegNode& reg) {
            return std::any_of(reg.accessors.begin(), reg.accessors.end(),
                [](const AccessorNode& accessor) {
                    return accessor.kind == AccessorKind::Index;
                });
        };
        if (!pointerModel.integral) {
            require(command.targetKind != TargetKind::Register
                    || !hasPointerIndex(command.targetRegister),
                "indexed pointer access requires an integral pointer representation");
            for (const ValueNode* value : command.values) {
                require(value->kind != ValueKind::Register
                        || !hasPointerIndex(value->registerValue),
                    "indexed pointer access requires an integral pointer representation");
            }
        }
        if (command.targetKind == TargetKind::Register && !command.hasDeclaredType) {
            const std::string& root = command.targetRegister.name;
            const bool hasIndex = std::any_of(command.targetRegister.accessors.begin(),
                command.targetRegister.accessors.end(), [](const AccessorNode& accessor) {
                    return accessor.kind == AccessorKind::Index;
                });
            if (locals.find(root) == locals.end()) {
                const auto global = env.globals.find(root);
                if (env.constantObjects.find(root) != env.constantObjects.end()) {
                    const bool writesThroughPointer = global != env.globals.end()
                        && global->second.pointerLevel > 0 && hasIndex;
                    require(writesThroughPointer,
                        "cannot assign to read-only constant object '" + root + "'");
                }
                require(!(hasIndex && env.immutableGlobalBacking.find(root)
                            != env.immutableGlobalBacking.end()),
                    "cannot modify immutable global array or string storage through '" + root + "'");
            }
            require(!(hasIndex && immutableAliases.find(root) != immutableAliases.end()),
                "cannot modify immutable global array or string storage through '" + root + "'");
        }
        if (command.hasDeclaredType) {
            validateType(command.declaredType);
            if (command.declaredType.isView) {
                const std::string initializer = normalizedInstructionName(
                    command.instruction.name);
                const bool isViewSlice = command.hasInstruction && command.instruction.isSpecial
                    && (initializer == "viewslice" || initializer == "view_slice");
                require(!command.values.empty() && (!command.hasInstruction || isViewSlice),
                    "a bounded view must be initialized from an array literal, another bounded view, or #viewslice");
            }
            require(locals.find(command.targetRegister.name) == locals.end(),
                "duplicate local declaration '" + command.targetRegister.name + "'");
        }
        if (command.hasInstruction) {
            validateInstruction(command, labels);
            validatePrimaryOperandTypes(command, locals, env, pointerModel);
            validateExplicitCast(command, locals, env, pointerModel);
            const std::string& op = command.instruction.name;
            if (command.instruction.isSpecial) {
                const std::string special = normalizedInstructionName(op);
                constexpr std::array<std::string_view, 25> nativeSpecials = {
                    "pause", "lfence", "sfence", "mfence", "hlt", "cli", "sti",
                    "yield", "dmb", "dsb", "isb", "wfi", "wfe", "sev", "sevl",
                    "ecall", "ebreak", "fence_i", "sync", "lwsync", "isync",
                    "eieio", "dbar", "ibar", "serialize"};
                if (contains(nativeSpecials, special)) {
                    require(supportsNativeSpecialInstruction(
                                pointerModel.architecture, special),
                        "'#" + special + "' is unavailable on the selected target");
                    require(command.values.empty(), "'#" + special + "' takes no operands");
                    require(command.targetKind == TargetKind::None,
                        "'#" + special + "' does not produce a result");
                }
                if (special == "rdtsc") {
                    require(supportsNativeSpecialInstruction(
                                pointerModel.architecture, special),
                        "'#rdtsc' is only available on x86 targets");
                    require(command.values.empty(), "'#rdtsc' takes no operands");
                }
                if (special == "syscall") {
                    require(pointerModel.operatingSystem == OperatingSystem::Linux
                            && supportsLinuxSyscall(pointerModel.architecture),
                        "'#syscall' is available only for supported Linux target profiles");
                    require(!(pointerModel.architecture == Architecture::X86_64
                                && pointerModel.bits == 32),
                        "'#syscall' is not available for the Linux x32 ABI");
                    const unsigned requiredWordBits =
                        linuxSyscallWordBits(pointerModel.architecture);
                    require(pointerModel.bits == requiredWordBits,
                        "'#syscall' requires the " + std::to_string(requiredWordBits)
                            + "-bit Linux ABI for this architecture; the selected profile has "
                            + std::to_string(pointerModel.bits) + "-bit pointers");
                    require(command.values.size() >= 1 && command.values.size() <= 7,
                        "'#syscall' requires a number and zero to six arguments");
                    for (const ValueNode* value : command.values) {
                        const ValueType operand = inferValueType(*value, locals, env);
                        require(operand.known && isIntegerType(operand.type),
                            "'#syscall' operands must be scalar integers");
                    }
                }
                if (special == "inb" || special == "outb")
                    require(supportsNativeSpecialInstruction(
                                pointerModel.architecture, special),
                        "'#" + special + "' requires an x86 target");
                if (special == "fence_i")
                    require(targetFeatureEnabled(
                                pointerModel.effectiveTargetFeatures, "zifencei"),
                        "'#fence.i' requires the Zifencei target feature");
                const bool atomicRead = special == "atomicload";
                const bool atomicWrite = special == "atomicstore";
                const bool compareExchange = special == "cas" || special == "atomic_cas";
                const bool atomicRmw = special.rfind("atomic", 0) == 0
                    && !atomicRead && !atomicWrite && !compareExchange;
                const bool viewLength = special == "viewlen" || special == "view_len";
                const bool viewLoad = special == "viewload" || special == "view_load";
                const bool viewStore = special == "viewstore" || special == "view_store";
                const bool viewSlice = special == "viewslice" || special == "view_slice";
                if (special == "clz" || special == "ctz" || special == "popcnt"
                    || special == "bswap" || special == "bitreverse") {
                    require(command.values.size() == 1,
                        "'#" + special + "' requires exactly one operand");
                    const ValueType operand = inferValueType(*command.values[0], locals, env);
                    require(operand.known && isIntegerType(operand.type),
                        "'#" + special + "' requires a scalar integer operand");
                    if (special == "bswap" && operand.known && isIntegerType(operand.type)) {
                        const unsigned width = integerWidth(operand.type);
                        require(width == 16 || width == 32 || width == 64,
                            "'#bswap' requires a 16-, 32-, or 64-bit integer operand");
                    }
                }
                if (viewLength || viewLoad || viewStore || viewSlice) {
                    if (viewLoad || viewStore || viewSlice)
                        require(pointerModel.integral,
                            "bounded-view slicing and access require an integral pointer representation");
                    const std::size_t expected = viewLength ? 1u : viewLoad ? 2u : 3u;
                    require(command.values.size() == expected,
                        "'#" + special + "' requires " + std::to_string(expected) + " operand(s)");
                    require(command.values[0]->kind == ValueKind::Register
                            && command.values[0]->registerValue.accessors.empty(),
                        "'#" + special + "' requires a plain bounded-view register");
                    TypeNode view = registerType(command.values[0]->registerValue, locals, env);
                    require(view.isView, "'#" + special + "' requires a bounded view");
                    if (viewLength) {
                        require(command.targetKind == TargetKind::Register && command.hasDeclaredType
                                && command.declaredType.baseName == "u64"
                                && command.declaredType.pointerLevel == 0,
                            "'#viewlen' requires a u64 result");
                    } else if (viewSlice) {
                        ValueType start = inferValueType(*command.values[1], locals, env);
                        ValueType count = inferValueType(*command.values[2], locals, env);
                        require(start.known && isIntegerType(start.type)
                                && count.known && isIntegerType(count.type),
                            "'#viewslice' start and count must be integers");
                        require(command.targetKind == TargetKind::Register
                                && command.hasDeclaredType && sameType(view, command.declaredType),
                            "'#viewslice' result must have the source view's element type");
                    } else {
                        ValueType index = inferValueType(*command.values[1], locals, env);
                        require(index.known && isIntegerType(index.type),
                            "'#" + special + "' index must be an integer");
                        TypeNode element = view;
                        element.isView = false;
                        if (viewLoad) {
                            require(command.targetKind == TargetKind::Register
                                    && command.hasDeclaredType && sameType(element, command.declaredType),
                                "'#viewload' result type must match the view element type");
                        } else {
                            require(command.targetKind == TargetKind::None,
                                "'#viewstore' does not produce a result");
                            ValueType value = inferValueType(*command.values[2], locals, env);
                            require(value.known && sameType(element, value.type),
                                "'#viewstore' value type must match the view element type");
                        }
                    }
                }
                if (special == "viewstore" || special == "view_store")
                    require(command.targetKind == TargetKind::None,
                        "'#viewstore' does not produce a result");
                if (atomicRead || atomicWrite || compareExchange || atomicRmw) {
                    require(pointerModel.integral,
                        "atomic operations require an integral pointer representation so alignment can be checked");
                    require(!atomicRmw || contains(atomicInstructions, special),
                        "unsupported atomic instruction '#" + special + "'");
                    const std::size_t expectedCount = atomicRead ? 1 : atomicWrite ? 2
                        : compareExchange ? 3 : 2;
                    require(command.values.size() == expectedCount,
                        "incorrect operand count for '#" + special + "'");
                    ValueType pointerValue = inferValueType(*command.values[0], locals, env);
                    require(pointerValue.isNull
                            || (pointerValue.known && pointerValue.type.pointerLevel > 0),
                        "'#" + special + "' requires a pointer operand");
                    if (atomicWrite || compareExchange || atomicRmw)
                        require(!mayNameImmutableBacking(
                                    *command.values[0], locals, env, immutableAliases),
                            "'#" + special + "' cannot modify immutable global array or string storage");
                    TypeNode pointer;
                    if (!pointerValue.isNull) {
                        pointer = pointerValue.type;
                        --pointer.pointerLevel;
                    }
                    if (atomicRead) {
                        require(command.targetKind == TargetKind::Register && command.hasDeclaredType
                                && isAtomicLoadStoreType(command.declaredType),
                            "'#atomicload' supports scalar integers, f32/f64, and pointer types");
                        if (!pointerValue.isNull && pointer.pointerLevel > 0)
                            checkPointerConversion({command.declaredType, false, true}, pointer,
                                "'#atomicload' result");
                        require(command.hasDeclaredType
                                && (pointerValue.isNull || pointer.pointerLevel > 0
                                    || sameType(pointer, command.declaredType)),
                            "'#atomicload' result type must match the pointer's pointee type");
                    } else if (atomicWrite || atomicRmw) {
                        require(atomicWrite ? command.targetKind == TargetKind::None
                                            : command.targetKind == TargetKind::Register
                                                && command.hasDeclaredType,
                            atomicWrite ? "'#atomicstore' does not produce a result"
                                        : "atomic read-modify-write instructions require a typed result destination");
                        ValueType value = inferValueType(*command.values[1], locals, env);
                        if (atomicWrite)
                            require(!mayContainLocalPointer(
                                        *command.values[1], locals, env, localStorageAliases,
                                        localAggregateContents)
                                    || mayContainLocalPointer(
                                        *command.values[0], locals, env, localStorageAliases,
                                        localAggregateContents),
                                "cannot store a pointer or address derived from local storage in memory");
                        if (atomicWrite)
                            require(value.known && (isAtomicLoadStoreType(value.type)
                                    || (value.isNull && pointer.pointerLevel > 0)),
                                "'#atomicstore' supports scalar integers, f32/f64, and pointer types");
                        if (atomicRmw) {
                            const bool floatRmw = special == "atomicfadd" || special == "atomicfsub";
                            require(value.known && (floatRmw
                                    ? (value.type.baseName == "f32" || value.type.baseName == "f64")
                                    : isAtomicIntegerType(value.type)),
                                floatRmw ? "floating atomic RMW requires f32 or f64"
                                         : "integer atomic RMW requires an i/u8, i/u16, i/u32, or i/u64 value");
                        }
                        if (!pointerValue.isNull && pointer.pointerLevel > 0)
                            checkPointerConversion(value, pointer, "'#" + special + "' value operand");
                        require(value.known && (value.isNull ? pointer.pointerLevel > 0
                                    : pointerValue.isNull || pointer.pointerLevel > 0
                                        || sameType(value.type, pointer)),
                            "pointer type mismatch in '#" + special + "' value operand");
                        if (atomicRmw)
                            require(sameType(value.type, command.declaredType),
                                "atomic read-modify-write result type must match its value operand");
                    } else {
                        require(command.targetKind == TargetKind::Register && command.hasDeclaredType,
                            "'#cas' requires a typed result destination");
                        ValueType expected = inferValueType(*command.values[1], locals, env);
                        require(expected.known && isAtomicIntegerType(expected.type),
                            "'#cas' requires an i/u8, i/u16, i/u32, or i/u64 expected value");
                        require(sameType(expected.type, command.declaredType),
                            "'#cas' result type must match its expected value");
                        for (std::size_t index = 1; index < 3; ++index) {
                            ValueType value = inferValueType(*command.values[index], locals, env);
                            require(value.known && isAtomicIntegerType(value.type),
                                "'#cas' operands must be scalar integer values");
                            if (index == 1 && !pointerValue.isNull && pointer.pointerLevel > 0)
                                checkPointerConversion(value, pointer, "'#cas' comparison value");
                            require(index == 2 || (pointerValue.isNull || pointer.pointerLevel > 0
                                        || sameType(value.type, pointer)),
                                "pointer type mismatch in '#cas' comparison value");
                        }
                    }
                }
                if (special == "trap" || special == "nop" || special == "cpu_relax"
                    || special == "hlt" || special == "cli" || special == "sti")
                    require(command.values.empty(), "'#" + special + "' takes no operands");
                if (special == "trap" || special == "nop" || special == "cpu_relax"
                    || special == "hlt" || special == "cli" || special == "sti"
                    || special == "memcpy" || special == "memset" || special == "outb")
                    require(command.targetKind == TargetKind::None,
                        "'#" + special + "' does not produce a result");
                if (special == "memcpy" || special == "memset") {
                    require(pointerModel.integral,
                        "'#" + special + "' requires an integral pointer representation to check byte-range overflow");
                    require(command.values.size() == 3, "'#" + special + "' requires 3 operands");
                    const std::size_t pointerCount = special == "memcpy" ? 2 : 1;
                    for (std::size_t i = 0; i < pointerCount; ++i) {
                        ValueType pointer = inferValueType(*command.values[i], locals, env);
                        require(pointer.known && (pointer.isNull || pointer.type.pointerLevel > 0),
                            "'#" + special + "' pointer operand must be a pointer");
                    }
                    require(!mayNameImmutableBacking(
                                *command.values[0], locals, env, immutableAliases),
                        "'#" + special + "' cannot modify immutable global array or string storage");
                    if (special == "memcpy") {
                        require(!mayContainLocalPointer(
                                    *command.values[1], locals, env, localStorageAliases,
                                    localAggregateContents)
                                || mayContainLocalPointer(
                                    *command.values[0], locals, env, localStorageAliases,
                                    localAggregateContents),
                            "cannot copy bytes from local storage to a destination that may outlive the function");
                    }
                    ValueType size = inferValueType(*command.values[2], locals, env);
                    require(size.known && isIntegerType(size.type), "'#" + special + "' size must be an integer");
                    if (special == "memset") {
                        ValueType byte = inferValueType(*command.values[1], locals, env);
                        require(byte.known && isIntegerType(byte.type), "'#memset' byte value must be an integer");
                    }
                }
                if (special == "inb") {
                    require(command.values.size() == 1, "'#inb' requires one port operand");
                    require(command.hasDeclaredType && command.declaredType.baseName == "u8"
                            && command.declaredType.pointerLevel == 0,
                        "'#inb' requires an explicitly declared u8 result");
                }
                if (special == "outb")
                    require(command.values.size() == 2, "'#outb' requires port and value operands");
                if (special == "inb" || special == "outb") {
                    ValueType port = inferValueType(*command.values[0], locals, env);
                    require(port.known && isIntegerType(port.type), "'#" + special + "' port must be an integer");
                    if (special == "outb") {
                        ValueType value = inferValueType(*command.values[1], locals, env);
                        require(value.known && isIntegerType(value.type), "'#outb' value must be an integer");
                    }
                }
            }
            if (op == "ret" && !command.values.empty())
                require(!mayContainLocalPointer(
                            *command.values[0], locals, env, localStorageAliases,
                            localAggregateContents),
                    "cannot return a pointer or address derived from local storage");
            if (op == "ret" && returnType && !command.values.empty())
                validateValueForType(*command.values[0], *returnType, locals, env, "return");
            if (op == "store" && command.values.size() == 2) {
                require(pointerModel.integral,
                    "'store' requires an integral pointer representation for address-range checks");
                require(!mayNameImmutableBacking(
                            *command.values[0], locals, env, immutableAliases),
                    "'store' cannot modify immutable global array or string storage");
                require(!mayContainLocalPointer(
                            *command.values[1], locals, env, localStorageAliases,
                            localAggregateContents)
                        || mayContainLocalPointer(
                            *command.values[0], locals, env, localStorageAliases,
                            localAggregateContents),
                    "cannot store a pointer or address derived from local storage in memory");
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
                if (!pointerValue.isNull && pointee.pointerLevel > 0) {
                    checkPointerConversion(stored, pointee, "store");
                } else {
                    require(stored.known && (pointerValue.isNull
                                ? !stored.isNull
                                : (!stored.isNull && sameType(stored.type, pointee))),
                        "pointer type mismatch in store: value must match the pointer's pointee type");
                }
            }
            if (op == "load" && !command.values.empty()) {
                require(pointerModel.integral,
                    "'load' requires an integral pointer representation for address-range checks");
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
            if (op == "call") {
                for (std::size_t index = 1; index < command.values.size(); ++index)
                    require(!mayContainLocalPointer(
                                *command.values[index], locals, env, localStorageAliases,
                                localAggregateContents),
                        "cannot pass a pointer or address derived from local storage to a call");
            }
            if (op == "call" && !command.values.empty()
                && command.values[0]->kind == ValueKind::Register) {
                const std::string& callee = command.values[0]->registerValue.name;
                auto parameters = env.parameters.find(callee);
                if (parameters == env.parameters.end()) {
                    TypeNode functionPointer = registerType(command.values[0]->registerValue, locals, env);
                    require(functionPointer.pointerLevel > 0,
                        "indirect call requires a function pointer");
                    require(!command.values[0]->registerValue.accessors.size(),
                        "indirect call requires a plain function pointer register");
                    for (std::size_t index = 1; index < command.values.size(); ++index) {
                        ValueType argument = inferValueType(*command.values[index], locals, env);
                        require(!argument.known || !argument.type.isView,
                            "bounded views cannot be passed to an indirect call");
                    }
                }
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
                    ValueType result = inferCommandType(command, locals, env, pointerModel);
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
        const bool assignsGlobal = command.targetKind == TargetKind::Register
            && !command.hasDeclaredType
            && locals.find(command.targetRegister.name) == locals.end();
        require(!assignsGlobal
                || !commandMayPointIntoLocalStorage(
                    command, locals, env, localStorageAliases, localReferences,
                    localAggregateContents),
            "cannot store a pointer or address derived from local storage in a global");
        const bool assignsThroughAccessor = command.targetKind == TargetKind::Register
            && !command.targetRegister.accessors.empty();
        require(!assignsThroughAccessor
                || !commandMayPointIntoLocalStorage(
                    command, locals, env, localStorageAliases, localReferences,
                    localAggregateContents)
                || accessorTargetIsLocalStorage(
                    command.targetRegister, locals, localStorageAliases),
            "cannot store a pointer or address derived from local storage through an accessor unless the destination is proven local");
        if (command.hasInstruction && !command.instruction.isSpecial
            && command.instruction.name == "ref" && !command.values.empty()
            && command.values[0]->kind == ValueKind::Register) {
            const RegNode& referenced = command.values[0]->registerValue;
            const bool isLocal = locals.find(referenced.name) != locals.end();
            const bool referencesImmutableBacking = std::any_of(referenced.accessors.begin(),
                referenced.accessors.end(), [](const AccessorNode& accessor) {
                    return accessor.kind == AccessorKind::Index;
                }) && env.immutableGlobalBacking.find(referenced.name)
                    != env.immutableGlobalBacking.end();
            const bool referencesImmutableAlias = std::any_of(referenced.accessors.begin(),
                referenced.accessors.end(), [](const AccessorNode& accessor) {
                    return accessor.kind == AccessorKind::Index;
                }) && immutableAliases.find(referenced.name) != immutableAliases.end();
            require(isLocal || env.constantObjects.find(referenced.name)
                    == env.constantObjects.end(),
                "cannot take a writable reference to read-only constant object '"
                    + referenced.name + "'");
            require(!referencesImmutableBacking,
                "cannot take a writable reference to immutable global array or string storage");
            require(!referencesImmutableAlias,
                "cannot take a writable reference to storage that may be immutable");
            require(!registerType(referenced, locals, env).isView,
                "bounded view storage cannot be exposed through ref");
        }
        if (command.targetKind == TargetKind::Register
            && command.targetRegister.accessors.empty()
            && (command.hasDeclaredType
                || locals.find(command.targetRegister.name) != locals.end())) {
            const std::string instructionName = command.hasInstruction
                ? normalizedInstructionName(command.instruction.name) : std::string{};
            if (commandMayNameImmutableBacking(command, locals, env, immutableAliases))
                immutableAliases.insert(command.targetRegister.name);
        }
        if (command.hasDeclaredType)
            locals.emplace(command.targetRegister.name, command.declaredType);
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

void validateProgram(const ProgramNode& program, PointerModel pointerModel)
{
    activeLocation = {};
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
        case DeclKind::Glob:
            env.globals.emplace(decl.globDecl.name, decl.globDecl.type);
            if (decl.globDecl.value
                && (decl.globDecl.value->kind == ValueKind::Array
                    || decl.globDecl.value->kind == ValueKind::String))
                env.immutableGlobalBacking.insert(decl.globDecl.name);
            break;
        case DeclKind::Const:
            env.globals.emplace(decl.constDecl.name, decl.constDecl.type);
            env.constantObjects.insert(decl.constDecl.name);
            if (decl.constDecl.value
                && (decl.constDecl.value->kind == ValueKind::Array
                    || decl.constDecl.value->kind == ValueKind::String))
                env.immutableGlobalBacking.insert(decl.constDecl.name);
            break;
        case DeclKind::Entry: break;
        }
    }
    for (const auto& decl : program.declarations) {
        activeLocation = decl.location;
        try {
            std::string name;
            switch (decl.kind) {
            case DeclKind::Entry:
                name = decl.entryDecl.name;
                break;
            case DeclKind::Func: {
                const auto& fn = decl.funcDecl;
                name = fn.name;
                validateType(fn.returnType, true);
                require(!fn.returnType.isView, "bounded views cannot be returned from a function");
                std::unordered_map<std::string, TypeNode> args;
                for (const auto& arg : fn.args) {
                    validateType(arg.type);
                    require(args.emplace(arg.name, arg.type).second,
                        "duplicate argument '" + arg.name + "'");
                }
                if (!fn.hasBody)
                    for (const auto& arg : fn.args)
                        require(!arg.type.isView,
                            "bounded views cannot be passed to external function declarations");
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
                    require(!field.type.isView, "bounded views cannot be stored in structures");
                    require(fields.insert(field.name).second, "duplicate field '" + field.name + "'");
                }
                break;
            }
            case DeclKind::Glob:
                name = decl.globDecl.name;
                validateType(decl.globDecl.type);
                require(!decl.globDecl.type.isView, "bounded views cannot have global storage");
                if (decl.globDecl.value)
                    validateValueForType(*decl.globDecl.value, decl.globDecl.type, {}, env, "global initializer");
                break;
            case DeclKind::Const:
                name = decl.constDecl.name;
                validateType(decl.constDecl.type);
                require(!decl.constDecl.type.isView, "bounded views cannot have constant storage");
                if (decl.constDecl.value)
                    validateValueForType(*decl.constDecl.value, decl.constDecl.type, {}, env, "constant initializer");
                break;
            }
            require(symbols.insert(name).second, "duplicate declaration '" + name + "'");
        } catch (const LocatedValidationError&) {
            throw;
        } catch (const std::exception& error) {
            throw LocatedValidationError(error.what(), decl.location);
        }
    }
    validateStructCycles(structs);
    for (const auto& decl : program.declarations) {
        if (decl.kind == DeclKind::Entry) {
            try {
                validateBlock(decl.entryDecl.body, env, {}, nullptr, pointerModel);
            } catch (const std::exception& error) {
                throw LocatedValidationError(error.what(), activeLocation);
            }
        } else if (decl.kind == DeclKind::Func && decl.funcDecl.hasBody) {
            std::unordered_map<std::string, TypeNode> args;
            for (const auto& arg : decl.funcDecl.args) args.emplace(arg.name, arg.type);
            try {
                validateBlock(decl.funcDecl.body, env, std::move(args), &decl.funcDecl.returnType, pointerModel);
            } catch (const std::exception& error) {
                throw LocatedValidationError(error.what(), activeLocation);
            }
        }
    }
}
