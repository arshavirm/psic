#include "logging.hpp"

#include <iostream>

namespace psi {

namespace {
    thread_local bool verboseEnabled = false;
    thread_local int errors = 0;
    thread_local std::vector<psic::Diagnostic>* diagnosticSink = nullptr;
    thread_local std::size_t diagnosticLine = 0;
    thread_local std::size_t diagnosticColumn = 0;

    std::string stripTrailingNewline(std::string message)
    {
        if (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
        return message;
    }

    void emitDiagnostic(psic::DiagnosticSeverity severity, const std::string& message)
    {
        if (diagnosticSink) {
            diagnosticSink->push_back({severity, message, diagnosticLine, diagnosticColumn,
                {}, {}, false});
            return;
        }

        const char* label = "note";
        if (severity == psic::DiagnosticSeverity::Error) label = "error";
        else if (severity == psic::DiagnosticSeverity::Warning) label = "warning";
        std::cerr << "psic: " << label << ": " << message << "\n";
    }
}

DiagnosticLocationScope::DiagnosticLocationScope(std::size_t line, std::size_t column)
    : previousLine(diagnosticLine), previousColumn(diagnosticColumn)
{
    diagnosticLine = line;
    diagnosticColumn = column;
}

DiagnosticLocationScope::~DiagnosticLocationScope()
{
    diagnosticLine = previousLine;
    diagnosticColumn = previousColumn;
}

DiagnosticCaptureScope::DiagnosticCaptureScope(std::vector<psic::Diagnostic>& diagnostics)
    : previousSink(diagnosticSink), previousErrors(errors), previousLine(diagnosticLine),
      previousColumn(diagnosticColumn), previousVerbose(verboseEnabled)
{
    diagnosticSink = &diagnostics;
    errors = 0;
    diagnosticLine = 0;
    diagnosticColumn = 0;
}

DiagnosticCaptureScope::~DiagnosticCaptureScope()
{
    diagnosticSink = previousSink;
    errors = previousErrors;
    diagnosticLine = previousLine;
    diagnosticColumn = previousColumn;
    verboseEnabled = previousVerbose;
}

bool DiagnosticCaptureScope::hasErrors() const noexcept
{
    return errors > 0;
}

std::vector<psic::Diagnostic>* setDiagnosticSink(std::vector<psic::Diagnostic>* sink)
{
    auto* previous = diagnosticSink;
    diagnosticSink = sink;
    return previous;
}

void setVerbose(bool enabled)
{
    verboseEnabled = enabled;
}

void logError(const std::string& message)
{
    errors++;
    emitDiagnostic(psic::DiagnosticSeverity::Error, message);
}

void logError(const std::string& message, std::size_t line, std::size_t column)
{
    errors++;
    if (!line) line = diagnosticLine;
    if (!column) column = diagnosticColumn;
    if (diagnosticSink) {
        diagnosticSink->push_back({psic::DiagnosticSeverity::Error, message, line, column,
            {}, {}, false});
        return;
    }
    std::cerr << "psic: error: " << message << " (" << line << ':' << column << ")\n";
}

void logWarning(const std::string& message)
{
    emitDiagnostic(psic::DiagnosticSeverity::Warning, message);
}

void logNote(const std::string& message)
{
    emitDiagnostic(psic::DiagnosticSeverity::Note, message);
}

void logInfo(const std::string& message)
{
    if (verboseEnabled && !diagnosticSink) {
        std::cerr << "psic: " << message << "\n";
    }
}

bool hadErrors()
{
    return errors > 0;
}

int errorCount()
{
    return errors;
}

void resetErrors()
{
    errors = 0;
}

DiagnosticStream::~DiagnosticStream() noexcept
{
    try {
        const std::string message = stripTrailingNewline(buffer.str());
        switch (kind) {
        case detail::StreamKind::Error:
            logError(message);
            break;
        case detail::StreamKind::Warning:
            logWarning(message);
            break;
        case detail::StreamKind::Note:
            logNote(message);
            break;
        case detail::StreamKind::VerboseInfo:
            if (verboseEnabled && !diagnosticSink && !message.empty()) {
                std::cerr << "psic: " << message << "\n";
            }
            break;
        }
    } catch (...) {
        // Logging runs from a destructor and must not terminate an embedding
        // process if formatting or diagnostic capture runs out of memory.
        // Preserve failure state so compile() cannot report success silently.
        if (kind == detail::StreamKind::Error) ++errors;
    }
}

}
