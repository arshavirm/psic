#include "parser.hpp"
#include <stdexcept>

Parser::Parser(const std::vector<Token>& tokenList)
{
    tokens = tokenList;
    position = 0;
}

Token Parser::lookAheadToken(int howManyAhead)
{
    int index = position + howManyAhead;
    if (index >= (int)tokens.size()) {
        return tokens[tokens.size() - 1];
    }
    return tokens[index];
}

Token Parser::currentToken()
{
    return lookAheadToken(0);
}

Token Parser::consumeToken()
{
    Token token = currentToken();
    if (position + 1 < (int)tokens.size()) {
        position = position + 1;
    }
    return token;
}

bool Parser::checkTokenType(TokenType type)
{
    return currentToken().type == type;
}

bool Parser::checkTokenTypeAhead(TokenType type, int howManyAhead)
{
    return lookAheadToken(howManyAhead).type == type;
}

bool Parser::checkKeyword(const std::string& text)
{
    Token token = currentToken();
    return token.type == TokenType::Identifier && token.text == text;
}

bool Parser::isReservedWord(const std::string& text)
{
    return text == "true" || text == "false" || text == "null" || text == "nullptr";
}

Token Parser::expectTokenType(TokenType type, const std::string& what)
{
    if (!checkTokenType(type)) {
        raiseError("expected " + what);
    }

    if (type == TokenType::Identifier && isReservedWord(currentToken().text)) {
        raiseError("'" + currentToken().text + "' is a reserved word and can't be used as a name");
    }
    return consumeToken();
}

void Parser::raiseError(const std::string& message)
{
    Token token = currentToken();
    std::string fullMessage = "parse error at line " + std::to_string(token.line) + ", column " + std::to_string(token.column) + ": " + message + " (found '" + token.text + "')";
    throw std::runtime_error(fullMessage);
}

bool Parser::isInstructionKeyword(const std::string& text)
{
    std::vector<std::string> instructionNames = {
        "add",
        "sub",
        "mul",
        "div",
        "mod",
        "eq",
        "gt",
        "lt",
        "neq",
        "gte",
        "lte",
        "and",
        "or",
        "not",
        "xor",
        "lsh",
        "rsh",
        "land",
        "lor",
        "lnot",
        "ref",
        "load",
        "store",
        "jmp",
        "cjmp",
        "call",
        "ret",
        "label",
    };
    for (int i = 0; i < (int)instructionNames.size(); i++) {
        if (instructionNames[i] == text) {
            return true;
        }
    }
    return false;
}

bool Parser::isPrimitiveTypeName(const std::string& text)
{
    std::vector<std::string> primitiveNames = {
        "void",
        "bool",
        "i8",
        "i16",
        "i32",
        "i64",
        "u8",
        "u16",
        "u32",
        "u64",
        "f32",
        "f64",
    };
    for (int i = 0; i < (int)primitiveNames.size(); i++) {
        if (primitiveNames[i] == text) {
            return true;
        }
    }
    return false;
}

bool Parser::tokenStartsInstruction(const Token& token)
{
    if (token.type == TokenType::SpecialInstruction) {
        return true;
    }
    if (token.type == TokenType::Identifier && isInstructionKeyword(token.text)) {
        return true;
    }
    return false;
}

int Parser::expectIndexNumber(const std::string& what)
{
    Token token = expectTokenType(TokenType::Number, what);

    bool isHex = token.text.size() > 2 && token.text[0] == '0' && (token.text[1] == 'x' || token.text[1] == 'X');

    if (isHex) {
        for (size_t i = 2; i < token.text.size(); i++) {
            char c = token.text[i];
            bool isHexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!isHexDigit) {
                raiseError(what + " must be a plain whole number (no '-' or '.')");
            }
        }
        return std::stoi(token.text, nullptr, 16);
    }

    for (int i = 0; i < (int)token.text.size(); i++) {
        char c = token.text[i];
        if (c < '0' || c > '9') {
            raiseError(what + " must be a plain whole number (no '-' or '.')");
        }
    }
    return std::stoi(token.text);
}

TypeNode Parser::parseType()
{
    TypeNode type;

    Token baseToken = expectTokenType(TokenType::Identifier, "a type name");
    type.baseName = baseToken.text;

    while (checkTokenType(TokenType::Star)) {
        consumeToken();
        type.pointerLevel = type.pointerLevel + 1;
    }

    if (checkTokenType(TokenType::Colon)) {
        consumeToken();
        type.hasAlignment = true;
        type.alignment = expectIndexNumber("an alignment number");
    }

    type.isPrimitive = isPrimitiveTypeName(type.baseName);
    return type;
}

RegNode Parser::parseRegister()
{
    RegNode reg;

    Token nameToken = expectTokenType(TokenType::Identifier, "a register name");
    reg.name = nameToken.text;

    while (checkTokenType(TokenType::LeftBracket) || checkTokenType(TokenType::Dot)) {
        if (checkTokenType(TokenType::LeftBracket)) {
            consumeToken();
            AccessorNode accessor;
            accessor.kind = AccessorKind::Index;
            accessor.index = expectIndexNumber("an array index");
            expectTokenType(TokenType::RightBracket, "']'");
            reg.accessors.push_back(accessor);
        } else {
            consumeToken();
            Token fieldToken = expectTokenType(TokenType::Identifier, "a field name");
            AccessorNode accessor;
            accessor.kind = AccessorKind::Field;
            accessor.fieldName = fieldToken.text;
            reg.accessors.push_back(accessor);
        }
    }

    return reg;
}

InstNode Parser::parseInstruction()
{
    InstNode inst;

    if (checkTokenType(TokenType::SpecialInstruction)) {
        Token token = consumeToken();
        inst.name = token.text.substr(1);
        inst.isSpecial = true;
        return inst;
    }

    if (checkTokenType(TokenType::Identifier) && isInstructionKeyword(currentToken().text)) {
        Token token = consumeToken();
        inst.name = token.text;
        inst.isSpecial = false;
        return inst;
    }

    raiseError("expected an instruction");
    return inst;
}

static bool isHexDigitChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    return 10 + (c - 'A');
}

std::string Parser::decodeStringEscapes(const std::string& rawText)
{
    std::string inner = rawText.substr(1, rawText.size() - 2);
    std::string result;
    size_t i = 0;

    while (i < inner.size()) {
        if (inner[i] != '\\') {
            result += inner[i];
            i += 1;
            continue;
        }

        if (i + 1 >= inner.size()) {
            raiseError("string ends with a dangling '\\'");
        }

        char next = inner[i + 1];
        switch (next) {
        case '"':
            result += '"';
            i += 2;
            break;
        case '\\':
            result += '\\';
            i += 2;
            break;
        case 'n':
            result += '\n';
            i += 2;
            break;
        case 't':
            result += '\t';
            i += 2;
            break;
        case 'r':
            result += '\r';
            i += 2;
            break;
        case '0':
            result += '\0';
            i += 2;
            break;
        case 'x': {
            bool hasTwoHexDigits = i + 3 < inner.size()
                && isHexDigitChar(inner[i + 2]) && isHexDigitChar(inner[i + 3]);
            if (!hasTwoHexDigits) {
                raiseError("'\\x' in a string needs exactly two hex digits, e.g. '\\x41'");
            }
            int byte = hexDigitValue(inner[i + 2]) * 16 + hexDigitValue(inner[i + 3]);
            result += (char)byte;
            i += 4;
            break;
        }
        default:
            raiseError(std::string("unknown escape '\\") + next + "' in a string");
        }
    }

    return result;
}

ValueNode* Parser::parseValue()
{
    Token token = currentToken();

    if (token.type == TokenType::Number) {
        consumeToken();
        ValueNode* value = makeValue();
        value->kind = ValueKind::Number;

        size_t hexPrefixPos = (!token.text.empty() && token.text[0] == '-') ? 1 : 0;
        bool isHex = token.text.size() > hexPrefixPos + 1
            && token.text[hexPrefixPos] == '0'
            && (token.text[hexPrefixPos + 1] == 'x' || token.text[hexPrefixPos + 1] == 'X');

        bool hasDecimalPoint = !isHex && (token.text.find('.') != std::string::npos);
        if (hasDecimalPoint) {
            value->numberIsFloat = true;
            value->numberAsFloat = std::stod(token.text);
        } else {
            value->numberIsFloat = false;
            value->numberAsInt = isHex ? std::stoll(token.text, nullptr, 16) : std::stoll(token.text);
        }
        return value;
    }

    if (token.type == TokenType::String) {
        consumeToken();
        ValueNode* value = makeValue();
        value->kind = ValueKind::String;
        value->stringValue = decodeStringEscapes(token.text);
        return value;
    }

    if (token.type == TokenType::SpecialRegister) {
        consumeToken();
        ValueNode* value = makeValue();
        value->kind = ValueKind::SpecialRegister;
        value->specialRegisterValue.name = token.text.substr(1);
        return value;
    }

    if (token.type == TokenType::LeftBracket) {
        consumeToken();
        ValueNode* value = makeValue();
        value->kind = ValueKind::Array;
        while (!checkTokenType(TokenType::RightBracket)) {
            value->arrayValues.push_back(parseValue());
        }
        expectTokenType(TokenType::RightBracket, "']'");
        return value;
    }

    if (token.type == TokenType::Identifier) {

        if (token.text == "true" || token.text == "false") {
            consumeToken();
            ValueNode* value = makeValue();
            value->kind = ValueKind::Bool;
            value->boolValue = (token.text == "true");
            return value;
        }

        if (token.text == "null" || token.text == "nullptr") {
            consumeToken();
            ValueNode* value = makeValue();
            value->kind = ValueKind::Null;
            return value;
        }

        ValueNode* value = makeValue();
        value->kind = ValueKind::Register;
        value->registerValue = parseRegister();
        return value;
    }

    raiseError("expected a value (a number, string, register, array, or %register)");
    return nullptr;
}

CommandNode Parser::parseCommand()
{
    CommandNode command;

    if (checkTokenType(TokenType::Semicolon)) {
        consumeToken();
        command.isEmpty = true;
        return command;
    }

    if (checkTokenType(TokenType::SpecialRegister)) {
        Token token = consumeToken();
        command.targetKind = TargetKind::SpecialRegister;
        command.targetSpecialRegister.name = token.text.substr(1);

    } else if (tokenStartsInstruction(currentToken())) {

    } else if (checkTokenType(TokenType::Identifier)) {

        bool typeHasStarOrAlignment = checkTokenTypeAhead(TokenType::Star, 1) || checkTokenTypeAhead(TokenType::Colon, 1);
        bool looksLikeTypeThenRegister = checkTokenTypeAhead(TokenType::Identifier, 1);
        bool looksLikePlainAssignment = checkTokenTypeAhead(TokenType::Equals, 1);
        bool looksLikeAccessorAssignment = checkTokenTypeAhead(TokenType::LeftBracket, 1) || checkTokenTypeAhead(TokenType::Dot, 1);

        if (typeHasStarOrAlignment || looksLikeTypeThenRegister) {
            command.hasDeclaredType = true;
            command.declaredType = parseType();
            command.targetKind = TargetKind::Register;
            command.targetRegister = parseRegister();

        } else if (looksLikePlainAssignment) {
            if (isReservedWord(currentToken().text)) {
                raiseError("'" + currentToken().text + "' is a reserved word and can't be used as a name");
            }
            Token nameToken = consumeToken();
            command.targetKind = TargetKind::Register;
            command.targetRegister.name = nameToken.text;

        } else if (looksLikeAccessorAssignment) {
            command.targetKind = TargetKind::Register;
            command.targetRegister = parseRegister();

        } else {
            raiseError("unexpected token after identifier in a command");
        }

    } else {
        raiseError("unexpected token at the start of a command");
    }

    bool needsValue = true;
    if (command.targetKind != TargetKind::None) {
        if (command.hasDeclaredType && checkTokenType(TokenType::Semicolon)) {

            needsValue = false;
        } else {
            expectTokenType(TokenType::Equals, "'='");
        }
    }

    if (needsValue) {
        if (tokenStartsInstruction(currentToken())) {
            command.hasInstruction = true;
            command.instruction = parseInstruction();
            while (!checkTokenType(TokenType::Semicolon)) {
                command.values.push_back(parseValue());
            }
        } else if (!checkTokenType(TokenType::Semicolon)) {

            command.values.push_back(parseValue());
        }
    }

    expectTokenType(TokenType::Semicolon, "';'");
    return command;
}

BlockNode Parser::parseBlock()
{
    BlockNode block;
    expectTokenType(TokenType::LeftBrace, "'{'");
    while (!checkTokenType(TokenType::RightBrace)) {
        block.commands.push_back(parseCommand());
    }
    expectTokenType(TokenType::RightBrace, "'}'");
    return block;
}

ArgNode Parser::parseArgument()
{
    ArgNode arg;
    arg.type = parseType();
    Token nameToken = expectTokenType(TokenType::Identifier, "an argument name");
    arg.name = nameToken.text;
    return arg;
}

StructDeclNode Parser::parseStructDeclaration()
{
    StructDeclNode decl;
    decl.type = parseType();
    expectTokenType(TokenType::LeftBrace, "'{'");
    while (!checkTokenType(TokenType::RightBrace)) {
        decl.fields.push_back(parseArgument());
    }
    expectTokenType(TokenType::RightBrace, "'}'");
    return decl;
}

FuncDeclNode Parser::parseFuncDeclaration()
{
    FuncDeclNode decl;
    decl.returnType = parseType();
    Token nameToken = expectTokenType(TokenType::Identifier, "a function name");
    decl.name = nameToken.text;
    while (!checkTokenType(TokenType::LeftBrace) && !checkTokenType(TokenType::Semicolon)) {
        decl.args.push_back(parseArgument());
    }

    if (checkTokenType(TokenType::Semicolon)) {
        consumeToken();
        decl.hasBody = false;
    } else {
        decl.hasBody = true;
        decl.body = parseBlock();
    }

    return decl;
}

GlobDeclNode Parser::parseGlobDeclaration()
{
    GlobDeclNode decl;
    decl.type = parseType();
    Token nameToken = expectTokenType(TokenType::Identifier, "a global name");
    decl.name = nameToken.text;
    expectTokenType(TokenType::Equals, "'='");
    decl.value = parseValue();
    expectTokenType(TokenType::Semicolon, "';'");
    return decl;
}

ConstDeclNode Parser::parseConstDeclaration()
{
    ConstDeclNode decl;
    decl.type = parseType();
    Token nameToken = expectTokenType(TokenType::Identifier, "a constant name");
    decl.name = nameToken.text;
    expectTokenType(TokenType::Equals, "'='");
    decl.value = parseValue();
    expectTokenType(TokenType::Semicolon, "';'");
    return decl;
}

DeclarationNode Parser::parseDeclaration()
{
    DeclarationNode decl;

    if (checkKeyword("entry")) {
        consumeToken();
        decl.kind = DeclKind::Entry;
        Token nameToken = expectTokenType(TokenType::Identifier, "an entry name");
        decl.entryDecl.name = nameToken.text;
        decl.entryDecl.body = parseBlock();
        return decl;
    }
    if (checkKeyword("struct")) {
        consumeToken();
        decl.kind = DeclKind::Struct;
        decl.structDecl = parseStructDeclaration();
        return decl;
    }
    if (checkKeyword("func")) {
        consumeToken();
        decl.kind = DeclKind::Func;
        decl.funcDecl = parseFuncDeclaration();
        return decl;
    }
    if (checkKeyword("const")) {
        consumeToken();
        decl.kind = DeclKind::Const;
        decl.constDecl = parseConstDeclaration();
        return decl;
    }

    decl.kind = DeclKind::Glob;
    decl.globDecl = parseGlobDeclaration();
    return decl;
}

ValueNode* Parser::makeValue()
{
    ownedValues.push_back(std::make_shared<ValueNode>());
    return ownedValues.back().get();
}

ProgramNode Parser::parseProgram()
{
    ProgramNode program;
    while (!checkTokenType(TokenType::EndOfFile)) {
        program.declarations.push_back(parseDeclaration());
    }
    program.ownedValues = std::move(ownedValues);
    return program;
}