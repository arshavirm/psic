#pragma once

#include <sstream>
#include <string>
#include <psic/compiler.hpp>

namespace psi {

std::vector<psic::Diagnostic>* setDiagnosticSink(std::vector<psic::Diagnostic>* sink);

void logError(const std::string& message);
void logWarning(const std::string& message);
void logNote(const std::string& message);
void logInfo(const std::string& message);

void setVerbose(bool enabled);

bool hadErrors();
int errorCount();
void resetErrors();

class ErrorStream {
public:
    ErrorStream() = default;
    ErrorStream(const ErrorStream&) = delete;
    ErrorStream& operator=(const ErrorStream&) = delete;
    ~ErrorStream();

    template <typename T>
    ErrorStream& operator<<(const T& value)
    {
        buffer << value;
        return *this;
    }

private:
    std::ostringstream buffer;
};

class InfoStream {
public:
    InfoStream() = default;
    InfoStream(const InfoStream&) = delete;
    InfoStream& operator=(const InfoStream&) = delete;
    ~InfoStream();

    template <typename T>
    InfoStream& operator<<(const T& value)
    {
        buffer << value;
        return *this;
    }

private:
    std::ostringstream buffer;
};

class WarningStream {
public:
    WarningStream() = default;
    WarningStream(const WarningStream&) = delete;
    WarningStream& operator=(const WarningStream&) = delete;
    ~WarningStream();

    template <typename T>
    WarningStream& operator<<(const T& value)
    {
        buffer << value;
        return *this;
    }

private:
    std::ostringstream buffer;
};

}