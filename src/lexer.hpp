#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "ast.hpp"

enum class TokenType {
    Identifier,
    Number,
    String,
    SpecialRegister,
    SpecialInstruction,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Star,
    Dot,
    Colon,
    Equals,
    Semicolon,
    Comma,
    EndOfFile,
};

struct Token {
    TokenType type = TokenType::EndOfFile;
    std::string text;
    int line = 1;
    int column = 1;
};

class Lexer {
public:
    explicit Lexer(std::string_view sourceCode);

    std::vector<Token> tokenize();

private:
    std::string_view source;
    std::size_t position = 0;
    int line = 1;
    int column = 1;
    std::size_t utf8ContinuationBytes = 0;
    bool previousWasCarriageReturn = false;

    bool isAtEnd();
    char currentChar();
    char lookAheadChar();
    char readChar();
    Token makeToken(TokenType type, std::string text, int startLine, int startColumn) const;

    void skipWhitespaceAndComments();

    Token readIdentifier();
    Token readNumber();
    Token readString();
    Token readSpecialToken(char marker, TokenType type);
};
