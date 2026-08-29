#pragma once

#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

class Parser {
public:
    Parser(const std::vector<Token>& tokenList);

    ProgramNode parseProgram();

private:
    std::vector<Token> tokens;
    int position = 0;

    Token currentToken();
    Token lookAheadToken(int howManyAhead);
    Token consumeToken();
    bool checkTokenType(TokenType type);
    bool checkTokenTypeAhead(TokenType type, int howManyAhead);
    bool checkKeyword(const std::string& text);
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