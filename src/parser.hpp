#pragma once

#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

/**
 * Parser for PSI Language
 * 
 * Converts a token stream into an Abstract Syntax Tree (AST).
 * The parser implements recursive descent parsing for the PSI grammar.
 */
class Parser {
public:
    /**
     * Constructs a parser for the given token stream.
     * @param tokenList Tokens produced by the Lexer
     */
    explicit Parser(const std::vector<Token>& tokenList);

    /**
     * Parses the entire token stream into a program AST.
     * @return Complete ProgramNode representing the parsed program
     * @throws std::runtime_error on syntax errors with position information
     */
    ProgramNode parseProgram();

private:
    std::vector<Token> tokens; ///< Token stream to parse
    int position = 0;          ///< Current token position
    
    /// Owned storage for ValueNodes created during parsing
    std::vector<std::shared_ptr<ValueNode>> ownedValues;
    
    /// Factory method that allocates and tracks a new ValueNode
    ValueNode* makeValue();

    // Token navigation methods
    
    /// Returns current token without advancing
    Token currentToken();
    
    /// Returns token at offset without advancing
    Token lookAheadToken(int howManyAhead);
    
    /// Consumes and returns current token, advancing position
    Token consumeToken();
    
    /// Returns true if current token matches expected type
    bool checkTokenType(TokenType type);
    
    /// Returns true if token at offset matches expected type
    bool checkTokenTypeAhead(TokenType type, int howManyAhead);
    
    /// Returns true if current token is an identifier with given text
    bool checkKeyword(const std::string& text);
    
    /// Consumes current token if it matches, otherwise raises error
    Token expectTokenType(TokenType type, const std::string& what);
    
    /// Raises a parse error with current token position
    void raiseError(const std::string& message);

    // Parsing methods for each grammar construct
    
    /// Parses a top-level declaration (entry/struct/func/glob/const)
    DeclarationNode parseDeclaration();
    
    /// Parses a struct type declaration
    StructDeclNode parseStructDeclaration();
    
    /// Parses a function declaration (with or without body)
    FuncDeclNode parseFuncDeclaration();
    
    /// Parses a global variable declaration
    GlobDeclNode parseGlobDeclaration();
    
    /// Parses a constant declaration
    ConstDeclNode parseConstDeclaration();
    
    /// Parses a function or field argument (type + name)
    ArgNode parseArgument();
    
    /// Parses a basic block (sequence of commands in braces)
    BlockNode parseBlock();
    
    /// Parses a single command/statement
    CommandNode parseCommand();
    
    /// Parses a value expression (number/string/register/array/etc.)
    ValueNode* parseValue();
    
    /// Parses a type annotation (base type, pointers, alignment)
    TypeNode parseType();
    
    /// Parses a register reference with optional accessors
    RegNode parseRegister();
    
    /// Parses an instruction (standard or special #instruction)
    InstNode parseInstruction();
    
    /// Parses an index number for array/field access
    int expectIndexNumber(const std::string& what);

    // Helper predicates
    
    /// Returns true if text is a built-in instruction keyword
    bool isInstructionKeyword(const std::string& text);
    
    /// Returns true if text is a primitive type name
    bool isPrimitiveTypeName(const std::string& text);
    
    /// Returns true if token starts an instruction
    bool tokenStartsInstruction(const Token& token);
    
    /// Returns true if text is a reserved word (true/false/null)
    bool isReservedWord(const std::string& text);
    
    /// Decodes escape sequences in a string literal
    std::string decodeStringEscapes(const std::string& rawText);
};