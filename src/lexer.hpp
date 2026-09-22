#pragma once

#include <string>
#include <vector>

/**
 * Lexical Analyzer (Lexer) for PSI Language
 * 
 * Converts source code text into a sequence of tokens for parsing.
 * Handles identifiers, numbers, strings, special registers (%name),
 * special instructions (#name), and punctuation.
 */

/// Token types recognized by the lexer
enum class TokenType {
    Identifier,        ///< Variable/function names, keywords
    Number,            ///< Integer or floating-point literals
    String,            ///< Quoted string literals
    SpecialRegister,   ///< %name - target-specific register
    SpecialInstruction,///< #name - target-specific instruction
    LeftBrace,         ///< {
    RightBrace,        ///< }
    LeftBracket,       ///< [
    RightBracket,      ///< ]
    Star,              ///< *
    Dot,               ///< .
    Colon,             ///< :
    Equals,            ///< =
    Semicolon,         ///< ;
    EndOfFile,         ///< End of input
};

/**
 * Represents a single lexical token with position information.
 */
struct Token {
    TokenType type = TokenType::EndOfFile; ///< Token kind
    std::string text;                      ///< Original source text
    int line = 1;                          ///< 1-based line number
    int column = 1;                        ///< 1-based column number
};

/**
 * Lexer class - converts source code to token stream.
 * 
 * The lexer performs:
 * - Whitespace and comment skipping (// and /* style)
 * - Token recognition and classification
 * - Position tracking for error reporting
 */
class Lexer {
public:
    /**
     * Constructs a lexer for the given source code.
     * @param sourceCode The PSI source code to tokenize
     */
    explicit Lexer(const std::string& sourceCode);

    /**
     * Tokenizes the entire source code.
     * @return Vector of tokens including EOF marker
     * @throws std::runtime_error on invalid syntax (unterminated strings, etc.)
     */
    std::vector<Token> tokenize();

private:
    std::string source; ///< Source code being tokenized
    int position = 0;   ///< Current character position
    int line = 1;       ///< Current line number (1-based)
    int column = 1;     ///< Current column number (1-based)

    /// Returns true if position is past the end of source
    bool isAtEnd();
    
    /// Returns current character without advancing
    char currentChar();
    
    /// Returns next character without advancing
    char lookAheadChar();
    
    /// Reads and returns current character, advancing position
    char readChar();

    /// Skips whitespace and comments (both // and /* */)
    void skipWhitespaceAndComments();

    /// Reads an identifier or keyword
    Token readIdentifier();
    
    /// Reads a numeric literal (integer or float, decimal or hex)
    Token readNumber();
    
    /// Reads a quoted string literal
    Token readString();
    
    /// Reads a special token starting with % or #
    Token readSpecialToken(char marker, TokenType type);
};