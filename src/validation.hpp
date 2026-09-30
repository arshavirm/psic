#pragma once
#include "ast.hpp"
#include "target_model.hpp"
#include <stdexcept>

class LocatedValidationError : public std::runtime_error {
public:
    LocatedValidationError(const std::string& message, SourceLocation location)
        : std::runtime_error(message), location(location) {}

    SourceLocation location;
};

void validateProgram(const ProgramNode& program, PointerModel pointerModel);
