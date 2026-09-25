#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

class Parser {
public:
    explicit Parser(std::vector<Token> tokenList);

    ProgramNode parseProgram();

private:
    std::vector<Token> tokens;
    std::size_t position = 0;
    std::vector<std::shared_ptr<ValueNode>> ownedValues;
    ValueNode* makeValue();

    const Token& currentToken() const;
    const Token& lookAheadToken(std::size_t howManyAhead) const;
    Token consumeToken();
    bool checkTokenType(TokenType type) const;
    bool checkTokenTypeAhead(TokenType type, std::size_t howManyAhead) const;
    bool checkKeyword(const std::string& text) const;
    Token expectTokenType(TokenType type, const std::string& what);
    void raiseError(const std::string& message);

    DeclarationNode parseDeclaration();
    StructDeclNode parseStructDeclaration();
    FuncDeclNode parseFuncDeclaration();
    GlobDeclNode parseGlobDeclaration();
    ConstDeclNode parseConstDeclaration();
    ArgNode parseArgument();
    BlockNode parseBlock();
    CommandNode parseCommand();
    ValueNode* parseValue();
    TypeNode parseType();
    RegNode parseRegister();
    InstNode parseInstruction();
    int expectIndexNumber(const std::string& what);

    bool isInstructionKeyword(const std::string& text);
    bool isPrimitiveTypeName(const std::string& text);
    bool tokenStartsInstruction(const Token& token);
    bool isReservedWord(const std::string& text);
    std::string decodeStringEscapes(const std::string& rawText);
};
