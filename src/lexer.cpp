#include "lexer.hpp"
#include <stdexcept>

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
{
    source = sourceCode;
    position = 0;
    line = 1;
    column = 1;
}

bool Lexer::isAtEnd()
{
    return position >= (int)source.size();
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
    int nextPosition = position + 1;
    if (nextPosition >= (int)source.size()) {
        return '\0';
    }
    return source[nextPosition];
}

char Lexer::readChar()
{
    char c = source[position];
    position = position + 1;
    if (c == '\n') {
        line = line + 1;
        column = 1;
    } else {
        column = column + 1;
    }
    return c;
}

void Lexer::skipWhitespaceAndComments()
{
    while (!isAtEnd()) {
        char c = currentChar();
        bool isWhitespace = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');

        if (isWhitespace) {
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
    int startLine = line;
    int startColumn = column;
    std::string text;

    while (!isAtEnd() && isLetterOrDigitChar(currentChar())) {
        text = text + readChar();
    }

    Token token;
    token.type = TokenType::Identifier;
    token.text = text;
    token.line = startLine;
    token.column = startColumn;
    return token;
}

Token Lexer::readNumber()
{

    int startLine = line;
    int startColumn = column;
    std::string text;

    if (currentChar() == '-') {
        text = text + readChar();
    }

    bool isHex = (currentChar() == '0' && (lookAheadChar() == 'x' || lookAheadChar() == 'X'));
    if (isHex) {
        text = text + readChar();
        text = text + readChar();
        if (!isHexDigitChar(currentChar()))
            throw std::runtime_error("expected hexadecimal digits at line " + std::to_string(startLine));
        while (!isAtEnd() && isHexDigitChar(currentChar())) {
            text = text + readChar();
        }
    } else {
        while (!isAtEnd() && isDigitChar(currentChar())) {
            text = text + readChar();
        }
        if (currentChar() == '.' && isDigitChar(lookAheadChar())) {
            text = text + readChar();
            while (!isAtEnd() && isDigitChar(currentChar())) {
                text = text + readChar();
            }
        }
    }

    Token token;
    token.type = TokenType::Number;
    token.text = text;
    token.line = startLine;
    token.column = startColumn;
    return token;
}

Token Lexer::readString()
{

    int startLine = line;
    int startColumn = column;
    std::string text;
    text = text + readChar();

    while (true) {
        if (isAtEnd()) {
            throw std::runtime_error("unterminated string literal starting at line " + std::to_string(startLine));
        }
        char c = readChar();
        text = text + c;

        if (c == '"') {

            int backslashCount = 0;
            int i = (int)text.size() - 2;
            while (i >= 0 && text[i] == '\\') {
                backslashCount = backslashCount + 1;
                i = i - 1;
            }
            bool quoteIsEscaped = (backslashCount % 2 == 1);
            if (!quoteIsEscaped) {
                break;
            }
        }
    }

    Token token;
    token.type = TokenType::String;
    token.text = text;
    token.line = startLine;
    token.column = startColumn;
    return token;
}

Token Lexer::readSpecialToken(char marker, TokenType type)
{
    int startLine = line;
    int startColumn = column;
    std::string text;
    text = text + readChar();

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

    Token token;
    token.type = type;
    token.text = text;
    token.line = startLine;
    token.column = startColumn;
    return token;
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;

    while (true) {
        skipWhitespaceAndComments();

        if (isAtEnd()) {
            Token endToken;
            endToken.type = TokenType::EndOfFile;
            endToken.text = "";
            endToken.line = line;
            endToken.column = column;
            tokens.push_back(endToken);
            break;
        }

        char c = currentChar();
        int startLine = line;
        int startColumn = column;

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
        Token token;
        token.text = std::string(1, c);
        token.line = startLine;
        token.column = startColumn;

        if (c == '{') {
            token.type = TokenType::LeftBrace;
        } else if (c == '}') {
            token.type = TokenType::RightBrace;
        } else if (c == '[') {
            token.type = TokenType::LeftBracket;
        } else if (c == ']') {
            token.type = TokenType::RightBracket;
        } else if (c == '*') {
            token.type = TokenType::Star;
        } else if (c == '.') {
            token.type = TokenType::Dot;
        } else if (c == ':') {
            token.type = TokenType::Colon;
        } else if (c == '=') {
            token.type = TokenType::Equals;
        } else if (c == ';') {
            token.type = TokenType::Semicolon;
        } else {
            throw std::runtime_error(std::string("unexpected character '") + c + "' at line " + std::to_string(startLine));
        }

        tokens.push_back(token);
    }

    return tokens;
}