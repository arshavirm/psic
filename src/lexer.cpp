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

static std::size_t utf8SequenceLength(std::string_view text, std::size_t offset)
{
    const std::size_t remaining = text.size() - offset;
    const auto first = static_cast<unsigned char>(text[offset]);
    const std::size_t length = first >= 0xF0 && first <= 0xF4 ? 4
        : first >= 0xE0 && first <= 0xEF ? 3
        : first >= 0xC2 && first <= 0xDF ? 2 : 1;
    if (length == 1 || length > remaining) return 1;

    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(text[offset + index]);
        if ((continuation & 0xC0) != 0x80) return 1;
        if (index == 1 && ((first == 0xE0 && continuation < 0xA0)
                || (first == 0xED && continuation >= 0xA0)
                || (first == 0xF0 && continuation < 0x90)
                || (first == 0xF4 && continuation >= 0x90)))
            return 1;
    }
    return length;
}

Lexer::Lexer(std::string_view sourceCode)
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
    const std::size_t byteOffset = position;
    char c = source[position];
    ++position;
    if (c == '\r') {
        ++line;
        column = 1;
        utf8ContinuationBytes = 0;
        previousWasCarriageReturn = true;
    } else if (c == '\n') {
        if (!previousWasCarriageReturn) ++line;
        column = 1;
        utf8ContinuationBytes = 0;
        previousWasCarriageReturn = false;
    } else {
        const auto byte = static_cast<unsigned char>(c);
        if (utf8ContinuationBytes != 0 && (byte & 0xC0) == 0x80) {
            --utf8ContinuationBytes;
        } else {
            utf8ContinuationBytes = 0;
            ++column;
            utf8ContinuationBytes = utf8SequenceLength(source, byteOffset) - 1;
        }
        previousWasCarriageReturn = false;
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

            while (!isAtEnd() && currentChar() != '\n' && currentChar() != '\r') {
                readChar();
            }
            continue;
        }

        if (c == '/' && lookAheadChar() == '*') {
            const int startLine = line;
            const int startColumn = column;
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
            if (!closed) throw LocatedSourceError("unterminated block comment",
                {static_cast<std::size_t>(startLine), static_cast<std::size_t>(startColumn)});
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
            throw LocatedSourceError("expected hexadecimal digits",
                {static_cast<std::size_t>(startLine), static_cast<std::size_t>(startColumn)});
        while (!isAtEnd() && isHexDigitChar(currentChar())) {
            text += readChar();
        }
    } else if (currentChar() == '0' && (lookAheadChar() == 'o' || lookAheadChar() == 'O')) {
        text += readChar(); text += readChar();
        if (currentChar() < '0' || currentChar() > '7')
            throw LocatedSourceError("expected octal digits",
                {static_cast<std::size_t>(startLine), static_cast<std::size_t>(startColumn)});
        while (!isAtEnd() && currentChar() >= '0' && currentChar() <= '7') text += readChar();
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
        throw LocatedSourceError("unterminated string literal",
            {static_cast<std::size_t>(startLine), static_cast<std::size_t>(startColumn)});

    return makeToken(TokenType::String, std::move(text), startLine, startColumn);
}

Token Lexer::readSpecialToken(char marker, TokenType type)
{
    const int startLine = line;
    const int startColumn = column;
    std::string text;
    text += readChar();

    if (!isLetterChar(currentChar())) {
        throw LocatedSourceError(std::string("expected a name right after '") + marker + "'",
            {static_cast<std::size_t>(startLine), static_cast<std::size_t>(startColumn)});
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
        case ',':
            type = TokenType::Comma;
            break;
        default:
            throw LocatedSourceError(std::string("unexpected character '") + c + "'",
                {static_cast<std::size_t>(startLine), static_cast<std::size_t>(startColumn)});
        }

        tokens.push_back(makeToken(type, std::string(1, c), startLine, startColumn));
    }

    return tokens;
}
