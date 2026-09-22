#pragma once

#include <sstream>
#include <string>
#include <vector>
#include <psic/compiler.hpp>

/**
 * Diagnostic Logging System for PSIC Compiler
 * 
 * Provides thread-local error/warning/note logging with optional
 * diagnostic collection for library consumers. When no diagnostic
 * sink is set, messages are printed to stderr.
 */
namespace psi {

/**
 * Sets the diagnostic sink for collecting diagnostics programmatically.
 * @param sink Vector to collect diagnostics, or nullptr to use stderr
 * @return Previous diagnostic sink
 */
std::vector<psic::Diagnostic>* setDiagnosticSink(std::vector<psic::Diagnostic>* sink);

/// Logs an error message (increments error count)
void logError(const std::string& message);

/// Logs a warning message (increments warning count)
void logWarning(const std::string& message);

/// Logs a note message
void logNote(const std::string& message);

/// Logs an info message (only if verbose mode is enabled)
void logInfo(const std::string& message);

/// Enables or disables verbose output
void setVerbose(bool enabled);

/// Returns true if any errors have been logged
bool hadErrors();

/// Returns the number of errors logged
int errorCount();

/// Resets error and warning counts to zero
void resetErrors();

namespace detail {

/// Internal enumeration for diagnostic stream kinds
enum class StreamKind {
    Error,       ///< Error severity
    Warning,     ///< Warning severity
    Note,        ///< Note severity
    VerboseInfo, ///< Info (only shown in verbose mode)
};

} // namespace detail

/**
 * Buffered diagnostic stream for fluent message construction.
 * 
 * Messages are buffered and dispatched to the appropriate log function
 * when the stream is destroyed. This allows multi-part messages to be
 * constructed fluently: ErrorStream() << "error: " << msg << '\n';
 */
class DiagnosticStream {
public:
    explicit DiagnosticStream(detail::StreamKind kind);
    DiagnosticStream(const DiagnosticStream&) = delete;
    DiagnosticStream& operator=(const DiagnosticStream&) = delete;
    ~DiagnosticStream();

    /// Appends value to the buffered message
    template <typename T>
    DiagnosticStream& operator<<(const T& value)
    {
        buffer << value;
        return *this;
    }

private:
    detail::StreamKind kind;      ///< Type of diagnostic
    std::ostringstream buffer;    ///< Buffered message text
};

// Specialized stream classes for each diagnostic severity level
// Only ErrorStream is used by the compiler core; others provided for symmetry

/// Stream for error messages
class ErrorStream : public DiagnosticStream {
public:
    ErrorStream() : DiagnosticStream(detail::StreamKind::Error) {}
};

/// Stream for warning messages
class WarningStream : public DiagnosticStream {
public:
    WarningStream() : DiagnosticStream(detail::StreamKind::Warning) {}
};

/// Stream for info/verbose messages
class InfoStream : public DiagnosticStream {
public:
    InfoStream() : DiagnosticStream(detail::StreamKind::VerboseInfo) {}
};

} // namespace psi
