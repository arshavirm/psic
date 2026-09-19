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

namespace detail {

enum class StreamKind {
    Error,
    Warning,
    Note,
    VerboseInfo,
};

} // namespace detail

// Buffered diagnostic stream. The buffered text (with one trailing newline
// stripped) is dispatched to the matching log function when the stream is
// destroyed, so `ErrorStream() << "..." << '\n';` logs in one piece.
class DiagnosticStream {
public:
    explicit DiagnosticStream(detail::StreamKind kind) : kind(kind) {}
    DiagnosticStream(const DiagnosticStream&) = delete;
    DiagnosticStream& operator=(const DiagnosticStream&) = delete;
    ~DiagnosticStream();

    template <typename T>
    DiagnosticStream& operator<<(const T& value)
    {
        buffer << value;
        return *this;
    }

private:
    detail::StreamKind kind;
    std::ostringstream buffer;
};

// Distinct names so call sites read like the diagnostic they emit. Only
// ErrorStream is used by the compiler core today; the others are kept for
// symmetry with logError/logWarning/logInfo.
class ErrorStream : public DiagnosticStream {
public:
    ErrorStream() : DiagnosticStream(detail::StreamKind::Error) {}
};

class WarningStream : public DiagnosticStream {
public:
    WarningStream() : DiagnosticStream(detail::StreamKind::Warning) {}
};

class InfoStream : public DiagnosticStream {
public:
    InfoStream() : DiagnosticStream(detail::StreamKind::VerboseInfo) {}
};

}
