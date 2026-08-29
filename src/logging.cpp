#include "logging.hpp"

#include <iostream>

namespace psi {

namespace {
    bool verboseEnabled = false;
    int errors = 0;
    int warnings = 0;
}

void setVerbose(bool enabled)
{
    verboseEnabled = enabled;
}

void logError(const std::string& message)
{
    errors++;
    std::cerr << "psic: error: " << message << "\n";
}

void logWarning(const std::string& message)
{
    warnings++;
    std::cerr << "psic: warning: " << message << "\n";
}

void logNote(const std::string& message)
{
    std::cerr << "psic: note: " << message << "\n";
}

void logInfo(const std::string& message)
{
    if (verboseEnabled) {
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

ErrorStream::~ErrorStream()
{
    std::string message = buffer.str();
    if (!message.empty() && message.back() == '\n') {
        message.pop_back();
    }
    logError(message);
}

InfoStream::~InfoStream()
{
    std::string message = buffer.str();
    if (!message.empty() && message.back() == '\n') {
        message.pop_back();
    }
    if (verboseEnabled && !message.empty()) {
        std::cerr << "psic: " << message << "\n";
    }
}

WarningStream::~WarningStream()
{
    std::string message = buffer.str();
    if (!message.empty() && message.back() == '\n') {
        message.pop_back();
    }
    logWarning(message);
}

}