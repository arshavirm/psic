#include "lexer.hpp"
#include <stdexcept>
#include <utility>

static bool isDigitChar(char c)
{
    return c >= '0' && c <= '9';
}

static bool isHexDigitChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool isLetterChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isLetterOrDigitChar(char c)
{
    return isLetterChar(c) || isDigitChar(c);
}

Lexer::Lexer(const std::string& sourceCode)
    : source(sourceCode)
{}

bool Lexer::isAtEnd()
{
    return position >= source.size();
}

char Lexer::currentChar()
{
    if (isAtEnd()) {
        return '\0';
    }
    return source[position];
}

char Lexer::lookAheadChar()
{
    return position + 1 < source.size() ? source[position + 1] : '\0';
}

char Lexer::readChar()
{
    char c = source[position];
    ++position;
    if (c == '\n') {
        ++line;
        column = 1;
    } else {
        ++column;
    }
    return c;
}

Token Lexer::makeToken(TokenType type, std::string text, int startLine, int startColumn) const
{
    return {type, std::move(text), startLine, startColumn};
}

void Lexer::skipWhitespaceAndComments()
{
    while (!isAtEnd()) {
        char c = currentChar();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            readChar();
            continue;
        }

        if (c == '/' && lookAheadChar() == '/') {
            readChar();
            readChar();

            while (!isAtEnd() && currentChar() != '\n') {
                readChar();
            }
            continue;
        }

        if (c == '/' && lookAheadChar() == '*') {
            const int startLine = line;
            bool closed = false;
            readChar();
            readChar();
            while (!isAtEnd()) {
                if (currentChar() == '*' && lookAheadChar() == '/') {
                    readChar();
                    readChar();
                    closed = true;
                    break;
                }
                readChar();
            }
            if (!closed) throw std::runtime_error("unterminated block comment at line " + std::to_string(startLine));
            continue;
        }

        break;
    }
}

Token Lexer::readIdentifier()
{
    const int startLine = line;
    const int startColumn = column;
    std::string text;

    while (!isAtEnd() && isLetterOrDigitChar(currentChar())) {
        text += readChar();
    }

    return makeToken(TokenType::Identifier, std::move(text), startLine, startColumn);
}

Token Lexer::readNumber()
{
    const int startLine = line;
    const int startColumn = column;
    std::string text;

    if (currentChar() == '-') {
        text += readChar();
    }

    if (currentChar() == '0' && (lookAheadChar() == 'x' || lookAheadChar() == 'X')) {
        text += readChar();
        text += readChar();
        if (!isHexDigitChar(currentChar()))
            throw std::runtime_error("expected hexadecimal digits at line " + std::to_string(startLine));
        while (!isAtEnd() && isHexDigitChar(currentChar())) {
            text += readChar();
        }
    } else {
        while (!isAtEnd() && isDigitChar(currentChar())) {
            text += readChar();
        }
        if (currentChar() == '.' && isDigitChar(lookAheadChar())) {
            text += readChar();
            while (!isAtEnd() && isDigitChar(currentChar())) {
                text += readChar();
            }
        }
    }

    return makeToken(TokenType::Number, std::move(text), startLine, startColumn);
}

Token Lexer::readString()
{
    const int startLine = line;
    const int startColumn = column;
    std::string text;
    text += readChar();
    bool escaped = false;
    bool closed = false;

    while (!isAtEnd()) {
        char c = readChar();
        text += c;

        if (c == '"' && !escaped) {
            closed = true;
            break;
        }

        escaped = c == '\\' && !escaped;
    }

    if (!closed)
        throw std::runtime_error("unterminated string literal starting at line " + std::to_string(startLine));

    return makeToken(TokenType::String, std::move(text), startLine, startColumn);
}

Token Lexer::readSpecialToken(char marker, TokenType type)
{
    const int startLine = line;
    const int startColumn = column;
    std::string text;
    text += readChar();

    if (!isLetterChar(currentChar())) {
        throw std::runtime_error(std::string("expected a name right after '") + marker + "' at line " + std::to_string(startLine));
    }
    while (!isAtEnd()) {
        if (isLetterOrDigitChar(currentChar())) {
            text += readChar();
        } else if (marker == '#' && currentChar() == '.' && isLetterChar(lookAheadChar())) {
            text += readChar();
        } else {
            break;
        }
    }

    return makeToken(type, std::move(text), startLine, startColumn);
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;

    while (true) {
        skipWhitespaceAndComments();

        if (isAtEnd()) {
            tokens.push_back(makeToken(TokenType::EndOfFile, "", line, column));
            break;
        }

        char c = currentChar();
        const int startLine = line;
        const int startColumn = column;

        if (isLetterChar(c)) {
            tokens.push_back(readIdentifier());
            continue;
        }
        bool looksLikeNumber = isDigitChar(c) || (c == '-' && isDigitChar(lookAheadChar()));
        if (looksLikeNumber) {
            tokens.push_back(readNumber());
            continue;
        }
        if (c == '"') {
            tokens.push_back(readString());
            continue;
        }
        if (c == '%') {
            tokens.push_back(readSpecialToken('%', TokenType::SpecialRegister));
            continue;
        }
        if (c == '#') {
            tokens.push_back(readSpecialToken('#', TokenType::SpecialInstruction));
            continue;
        }

        readChar();
        TokenType type;
        switch (c) {
        case '{':
            type = TokenType::LeftBrace;
            break;
        case '}':
            type = TokenType::RightBrace;
            break;
        case '[':
            type = TokenType::LeftBracket;
            break;
        case ']':
            type = TokenType::RightBracket;
            break;
        case '*':
            type = TokenType::Star;
            break;
        case '.':
            type = TokenType::Dot;
            break;
        case ':':
            type = TokenType::Colon;
            break;
        case '=':
            type = TokenType::Equals;
            break;
        case ';':
            type = TokenType::Semicolon;
            break;
        default:
            throw std::runtime_error(std::string("unexpected character '") + c + "' at line " + std::to_string(startLine));
        }

        tokens.push_back(makeToken(type, std::string(1, c), startLine, startColumn));
    }

    return tokens;
}
