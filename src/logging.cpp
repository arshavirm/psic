#include "logging.hpp"

#include <iostream>

namespace psi {

namespace {
    thread_local bool verboseEnabled = false;
    thread_local int errors = 0;
    thread_local int warnings = 0;
    thread_local std::vector<psic::Diagnostic>* diagnosticSink = nullptr;

    std::string stripTrailingNewline(std::string message)
    {
        if (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
        return message;
    }
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
    if (diagnosticSink) {
        diagnosticSink->push_back({psic::DiagnosticSeverity::Error, message});
        return;
    }
    std::cerr << "psic: error: " << message << "\n";
}

void logWarning(const std::string& message)
{
    warnings++;
    if (diagnosticSink) {
        diagnosticSink->push_back({psic::DiagnosticSeverity::Warning, message});
        return;
    }
    std::cerr << "psic: warning: " << message << "\n";
}

void logNote(const std::string& message)
{
    if (diagnosticSink) {
        diagnosticSink->push_back({psic::DiagnosticSeverity::Note, message});
        return;
    }
    std::cerr << "psic: note: " << message << "\n";
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
    warnings = 0;
}

DiagnosticStream::~DiagnosticStream()
{
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
}

}
