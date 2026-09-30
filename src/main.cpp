#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

#include <psic/compiler.hpp>
#include "logging.hpp"
#include "target.hpp"

using OptLevel = psic::OptimizationLevel;

const std::string BANNER = R"(
█████ █████ █████
█   █ █       █  
█████ █████   █  
█         █   █  
█     █████ █████ Program System Interface
)";

#ifndef PSIC_VERSION
#define PSIC_VERSION "unknown"
#endif
static constexpr const char* VERSION = "psic " PSIC_VERSION;

struct Options {
    std::string input;
    std::string output;
    std::string targetTriple;
    std::string targetCPU;
    std::string targetFeatures;
    std::string arch;
    std::string os;
    std::string entry = "main";
    std::string diagnosticFormat = "text";
    OptLevel opt = OptLevel::O2;

    bool emitIR = false;
    bool outputModeSpecified = false;
    bool outputPathSpecified = false;
    bool entrySpecified = false;
    bool jit = false;
    bool showHelp = false;
    bool showVersion = false;
    bool verbose = false;
};

static void printVersion()
{
    std::cout << VERSION << '\n';
}

static bool isOption(const std::string& arg)
{
    return arg.size() > 1 && arg[0] == '-';
}

namespace {

void printUsage(std::ostream& out);

enum class OptionAction {
    ShowHelp,
    ShowVersion,
    SetVerbose,
    EmitObject,
    EmitLLVM,
    SetOutput,
    SetTargetTriple,
    SetTargetCPU,
    SetTargetFeatures,
    SetArch,
    SetOs,
    SetOptLevel,
    SetJit,
    SetEntry,
    SetDiagnosticFormat,
};

struct OptionSpec {
    const char* name;
    OptionAction action;
    bool takesValue;
    OptLevel optimization = OptLevel::O2;
    const char* helpLabel = nullptr;
    const char* helpText = nullptr;
};

// All spellings of every command-line option. Value-taking options also
// accept a `--name=value` form, handled below from the same table.
constexpr OptionSpec optionSpecs[] = {
    {"-h", OptionAction::ShowHelp, false, OptLevel::O2, "-h, --help", "Show this help message"},
    {"--help", OptionAction::ShowHelp, false},
    {"--version", OptionAction::ShowVersion, false, OptLevel::O2, "--version", "Show compiler version"},
    {"-v", OptionAction::SetVerbose, false, OptLevel::O2, "-v, --verbose", "Print compilation details, including target CPU and feature overrides"},
    {"--verbose", OptionAction::SetVerbose, false},
    {"-c", OptionAction::EmitObject, false, OptLevel::O2, "-c, --compile", "Compile to an object file (default; conflicts with --emit-llvm)"},
    {"--compile", OptionAction::EmitObject, false},
    {"--emit-llvm", OptionAction::EmitLLVM, false, OptLevel::O2, "--emit-llvm", "Write LLVM IR text instead of an object file"},
    {"--jit", OptionAction::SetJit, false, OptLevel::O2, "--jit", "Execute a native entry point (default: main)"},
    {"--entry", OptionAction::SetEntry, true, OptLevel::O2, "--entry <name>", "Select the no-argument JIT entry function"},
    {"--diagnostic-format", OptionAction::SetDiagnosticFormat, true, OptLevel::O2, "--diagnostic-format <format>", "Format diagnostics as text (default) or JSON"},
    {"-o", OptionAction::SetOutput, true, OptLevel::O2, "-o, --output <file>", "Write output to <file>"},
    {"--output", OptionAction::SetOutput, true},
    {"-target", OptionAction::SetTargetTriple, true, OptLevel::O2, "-target, --target <triple>", "Set target triple"},
    {"--target", OptionAction::SetTargetTriple, true},
    {"-mcpu", OptionAction::SetTargetCPU, true, OptLevel::O2, "-mcpu, --cpu <name>", "Select an LLVM target CPU"},
    {"--cpu", OptionAction::SetTargetCPU, true},
    {"-mattr", OptionAction::SetTargetFeatures, true, OptLevel::O2, "-mattr, --features <list>", "Append comma-separated LLVM target features (repeatable; later entries override earlier ones)"},
    {"--features", OptionAction::SetTargetFeatures, true},
    {"-march", OptionAction::SetArch, true, OptLevel::O2, "-march, --arch <arch>", "Set target architecture"},
    {"--arch", OptionAction::SetArch, true},
    {"-mos", OptionAction::SetOs, true, OptLevel::O2, "-mos, --os <os>", "Set target OS"},
    {"--os", OptionAction::SetOs, true},
    {"-O0", OptionAction::SetOptLevel, false, OptLevel::O0, "-O0", "Disable optimization"},
    {"-O1", OptionAction::SetOptLevel, false, OptLevel::O1, "-O1", "Optimize minimally"},
    {"-O2", OptionAction::SetOptLevel, false, OptLevel::O2, "-O2", "Optimize (default)"},
    {"-O3", OptionAction::SetOptLevel, false, OptLevel::O3, "-O3", "Optimize aggressively"},
    {"-Os", OptionAction::SetOptLevel, false, OptLevel::Os, "-Os", "Optimize for size"},
    {"-Oz", OptionAction::SetOptLevel, false, OptLevel::Oz, "-Oz", "Optimize for minimum size"},
};

const OptionSpec* findOption(const std::string& arg)
{
    for (const auto& spec : optionSpecs)
        if (arg == spec.name)
            return &spec;
    return nullptr;
}

void printUsage(std::ostream& out)
{
    out << BANNER << "\n"
        << "Usage: psic [options] <input>\n\n"
        << "Compile a PSI source file into an object file or LLVM IR.\n\n"
        << "Options:\n";
    for (const auto& spec : optionSpecs) {
        if (!spec.helpLabel) continue;
        out << "  " << std::left << std::setw(38) << spec.helpLabel
            << spec.helpText << '\n';
    }

    out << "      --                    Stop parsing options; use '-' for stdin\n\n"
        << "Architectures:\n";
    for (const ArchitectureNameGroup& group : architectureNameGroups()) {
        std::string names(group.canonical);
        for (std::string_view alias : group.aliases)
            names += ", " + std::string(alias);
        out << "  " << std::left << std::setw(48) << names
            << group.description << '\n';
    }
    out << "\nOperating systems:\n";
    for (const OperatingSystemNameGroup& group : operatingSystemNameGroups()) {
        std::string names(group.canonical);
        for (std::string_view alias : group.aliases)
            names += ", " + std::string(alias);
        out << "  " << std::left << std::setw(48) << names
            << group.description << '\n';
    }

    out << "\nSupported profile groups:\n"
        << "  x86, x86_64, arm, aarch64: Linux, Darwin, Windows, or none\n"
        << "  wasm32, wasm64: none or WASI\n"
        << "  RISC-V, PowerPC, MIPS, LoongArch64, SystemZ: Linux or none\n"
        << "  --target also accepts LLVM-recognized architecture/OS triples\n"
        << "  (unmapped architectures expose portable PSI only; see the specification)\n\n"
        << "Examples:\n"
        << "  psic hello.psi\n"
        << "  psic hello.psi -o hello.o\n"
        << "  psic -c hello.psi -o build/hello.o\n"
        << "  psic --target x86_64-pc-linux-gnu hello.psi -o hello.o\n"
        << "  psic --target x86_64-pc-linux-gnu --cpu znver4 --features=+avx2 hello.psi -o hello.o\n"
        << "  psic -march x86_64 hello.psi -o hello.o\n"
        << "  psic -march aarch64 -mos darwin hello.psi -o hello.o\n"
        << "  psic --emit-llvm hello.psi -o hello.ll\n"
        << "  psic --jit --entry main program.psi\n";
}

bool requireValue(int argc, char** argv, int& i, const std::string& option,
    std::string& value)
{
    if (i + 1 >= argc) {
        psi::logError("option '" + option + "' requires an argument");
        return false;
    }

    value = argv[++i];
    if (value.empty() || (isOption(value) && value != "-")) {
        psi::logError("option '" + option + "' requires an argument");
        return false;
    }

    return true;
}

bool applyOption(const OptionSpec& spec, const std::string& value, Options& options)
{
    auto setConsistent = [](std::string& current, const std::string& requested,
                             const char* option) {
        if (!current.empty() && current != requested) {
            psi::logError(std::string("conflicting values supplied for '") + option + "'");
            return false;
        }
        current = requested;
        return true;
    };

    switch (spec.action) {
    case OptionAction::ShowHelp: options.showHelp = true; break;
    case OptionAction::ShowVersion: options.showVersion = true; break;
    case OptionAction::SetVerbose: options.verbose = true; break;
    case OptionAction::EmitObject:
    case OptionAction::EmitLLVM: {
        const bool wantsIR = spec.action == OptionAction::EmitLLVM;
        if (options.outputModeSpecified && options.emitIR != wantsIR) {
            psi::logError("--compile and --emit-llvm select different output modes; choose one");
            return false;
        }
        options.emitIR = wantsIR;
        options.outputModeSpecified = true;
        break;
    }
    case OptionAction::SetOutput:
        if (!setConsistent(options.output, value, "-o/--output")) return false;
        options.outputPathSpecified = true;
        break;
    case OptionAction::SetTargetTriple:
        if (!setConsistent(options.targetTriple, value, "--target")) return false;
        break;
    case OptionAction::SetTargetCPU:
        if (!setConsistent(options.targetCPU, value, "--cpu")) return false;
        break;
    case OptionAction::SetTargetFeatures:
        if (!options.targetFeatures.empty()) options.targetFeatures += ',';
        options.targetFeatures += value;
        break;
    case OptionAction::SetArch:
        if (!setConsistent(options.arch, value, "--arch")) return false;
        break;
    case OptionAction::SetOs:
        if (!setConsistent(options.os, value, "--os")) return false;
        break;
    case OptionAction::SetOptLevel: options.opt = spec.optimization; break;
    case OptionAction::SetJit: options.jit = true; break;
    case OptionAction::SetEntry:
        if (options.entrySpecified && options.entry != value) {
            psi::logError("conflicting values supplied for '--entry'");
            return false;
        }
        options.entry = value;
        options.entrySpecified = true;
        break;
    case OptionAction::SetDiagnosticFormat:
        if (value != "text" && value != "json") {
            psi::logError("--diagnostic-format must be 'text' or 'json'");
            return false;
        }
        options.diagnosticFormat = value;
        break;
    }
    return true;
}

} // namespace

static bool parseArguments(int argc, char** argv, Options& options)
{
    bool endOfOptions = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (!endOfOptions && arg == "--") {
            endOfOptions = true;
            continue;
        }

        if (!endOfOptions) {
            const OptionSpec* spec = findOption(arg);
            if (spec) {
                std::string value;
                if (spec->takesValue && !requireValue(argc, argv, i, arg, value)) return false;
                if (!applyOption(*spec, value, options)) return false;
                continue;
            }

            // `--name=value` forms reuse the same option table.
            const auto equals = arg.find('=');
            if (equals != std::string::npos) {
                const std::string name = arg.substr(0, equals);
                const OptionSpec* inlineSpec = findOption(name);
                if (inlineSpec && inlineSpec->takesValue) {
                    const std::string inlineValue = arg.substr(equals + 1);
                    if (inlineValue.empty()) {
                        psi::logError("'" + name + "=' requires an argument");
                        return false;
                    }
                    if (!applyOption(*inlineSpec, inlineValue, options)) return false;
                    continue;
                }
            }

            if (isOption(arg)) {
                psi::logError("unknown option '" + arg + "'");
                psi::logNote("use 'psic --help' for usage information");
                return false;
            }
        }

        if (!options.input.empty()) {
            psi::logError("multiple input files are not supported");
            return false;
        }

        options.input = arg;
    }

    if (options.jit && (options.outputModeSpecified || options.outputPathSpecified)) {
        psi::logError("--jit executes the program and cannot be combined with output options");
        return false;
    }
    if (options.entrySpecified && !options.jit) {
        psi::logError("--entry requires --jit");
        return false;
    }
    return true;
}

static void logTargetSubtarget(const Options& options)
{
    psi::logInfo("Target CPU: " + (options.targetCPU.empty() ? "generic" : options.targetCPU));
    if (!options.targetFeatures.empty())
        psi::logInfo("Target feature overrides: " + options.targetFeatures);
}

static std::string defaultOutputPath(const std::string& input, bool emitIR)
{
    if (input == "-")
        return emitIR ? "a.ll" : "a.o";

    std::filesystem::path path(input);
    path.replace_extension(emitIR ? ".ll" : ".o");
    return path.string();
}

static bool readSource(const std::string& input, std::string& source)
{
    if (input == "-") {
#if defined(_WIN32)
        if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
            psi::logError("cannot read standard input in binary mode");
            return false;
        }
#endif
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        source = buffer.str();
        return true;
    }

    std::ifstream file(input, std::ios::in | std::ios::binary);
    if (!file) {
        psi::logError("cannot open input file '" + input + "'");
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    source = buffer.str();
    return true;
}

static bool writeIR(const std::string& ir, const std::string& output)
{
    if (output == "-") {
        std::cout << ir;
        return static_cast<bool>(std::cout);
    }

    std::ofstream file(output);
    if (!file) {
        psi::logError("cannot open output file '" + output + "'");
        return false;
    }

    file << ir;
    if (!file) {
        psi::logError("error writing output file '" + output + "'");
        return false;
    }

    return true;
}

static bool writeObject(const std::vector<std::uint8_t>& object, const std::string& output)
{
    if (output == "-") {
        psi::logError("object output to stdout is not supported; specify an output file");
        return false;
    }

    std::ofstream file(output, std::ios::binary);
    if (!file) {
        psi::logError("cannot open output file '" + output + "'");
        return false;
    }

    file.write(reinterpret_cast<const char*>(object.data()), object.size());
    file.close();
    if (!file) {
        psi::logError("error writing output file '" + output + "'");
        return false;
    }
    return true;
}

static std::size_t utf8SequenceLength(const std::string& text, std::size_t offset,
    std::size_t end)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    std::size_t length = first >= 0xF0 && first <= 0xF4 ? 4
        : first >= 0xE0 && first <= 0xEF ? 3
        : first >= 0xC2 && first <= 0xDF ? 2 : 1;
    if (length == 1 || length > end - offset) return 1;

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

static void writeJsonString(std::ostream& out, const std::string& text)
{
    static constexpr char hex[] = "0123456789abcdef";
    out.put('"');
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        switch (byte) {
        case '"': out << "\\\""; ++index; continue;
        case '\\': out << "\\\\"; ++index; continue;
        case '\b': out << "\\b"; ++index; continue;
        case '\f': out << "\\f"; ++index; continue;
        case '\n': out << "\\n"; ++index; continue;
        case '\r': out << "\\r"; ++index; continue;
        case '\t': out << "\\t"; ++index; continue;
        default: break;
        }
        if (byte < 0x20) {
            out << "\\u00" << hex[byte >> 4] << hex[byte & 0x0f];
            ++index;
        } else if (byte >= 0x80) {
            const std::size_t length = utf8SequenceLength(text, index, text.size());
            if (length == 1) {
                out << "\\u00" << hex[byte >> 4] << hex[byte & 0x0f];
                ++index;
            } else {
                out.write(text.data() + index, static_cast<std::streamsize>(length));
                index += length;
            }
        } else {
            out.put(static_cast<char>(byte));
            ++index;
        }
    }
    out.put('"');
}

static void printJsonDiagnostics(const std::vector<psic::Diagnostic>& diagnostics,
    const std::string& sourceName)
{
    std::cerr << '[';
    bool first = true;
    for (const auto& diagnostic : diagnostics) {
        if (!first) std::cerr << ',';
        first = false;
        std::cerr << "{\"severity\":";
        switch (diagnostic.severity) {
        case psic::DiagnosticSeverity::Error: writeJsonString(std::cerr, "error"); break;
        case psic::DiagnosticSeverity::Warning: writeJsonString(std::cerr, "warning"); break;
        case psic::DiagnosticSeverity::Note: writeJsonString(std::cerr, "note"); break;
        }
        std::cerr << ",\"message\":";
        writeJsonString(std::cerr, diagnostic.message);
        std::cerr << ",\"source\":";
        writeJsonString(std::cerr, diagnostic.sourceName.empty()
            ? sourceName : diagnostic.sourceName);
        std::cerr << ",\"line\":" << diagnostic.line
                  << ",\"column\":" << diagnostic.column
                  << ",\"source_line\":";
        if (diagnostic.hasSourceLine) writeJsonString(std::cerr, diagnostic.sourceLine);
        else std::cerr << "null";
        std::cerr << '}';
    }
    std::cerr << "]\n";
}

static void printDiagnostics(const std::vector<psic::Diagnostic>& diagnostics,
    const std::string& source, const std::string& sourceName,
    const std::string& format)
{
    if (format == "json") {
        printJsonDiagnostics(diagnostics, sourceName);
        return;
    }
    for (const auto& diagnostic : diagnostics) {
        const std::string& file = diagnostic.sourceName.empty()
            ? sourceName : diagnostic.sourceName;
        const std::string location = diagnostic.line
            ? file + ":" + std::to_string(diagnostic.line) + ":"
                + std::to_string(diagnostic.column) + ": "
            : std::string{};
        switch (diagnostic.severity) {
        case psic::DiagnosticSeverity::Error: psi::logError(location + diagnostic.message); break;
        case psic::DiagnosticSeverity::Warning: psi::logWarning(location + diagnostic.message); break;
        case psic::DiagnosticSeverity::Note: psi::logNote(location + diagnostic.message); break;
        }
        if (!diagnostic.line) continue;

        std::size_t lineStart = 0;
        std::size_t currentLine = 1;
        while (currentLine < diagnostic.line && lineStart < source.size()) {
            if (source[lineStart] == '\r') {
                ++lineStart;
                if (lineStart < source.size() && source[lineStart] == '\n') ++lineStart;
                ++currentLine;
            } else if (source[lineStart++] == '\n') {
                ++currentLine;
            }
        }
        std::size_t lineEnd = lineStart;
        while (lineEnd < source.size() && source[lineEnd] != '\n' && source[lineEnd] != '\r')
            ++lineEnd;
        std::cerr << "  " << source.substr(lineStart, lineEnd - lineStart) << '\n';

        // Match terminal tab expansion in the displayed source line. The two
        // leading spaces printed above are included when calculating tab stops.
        const std::size_t sourceScalarsBefore = diagnostic.column > 0
            ? diagnostic.column - 1 : 0;
        std::string caretPrefix;
        std::size_t terminalColumn = 2;
        std::size_t sourceScalarsSeen = 0;
        for (std::size_t index = 0; index < lineEnd - lineStart
                && sourceScalarsSeen < sourceScalarsBefore;) {
            if (source[lineStart + index] == '\t') {
                const std::size_t nextStop = ((terminalColumn / 8) + 1) * 8;
                caretPrefix.append(nextStop - terminalColumn, ' ');
                terminalColumn = nextStop;
                ++index;
                ++sourceScalarsSeen;
            } else {
                const std::size_t byteOffset = lineStart + index;
                const std::size_t length = utf8SequenceLength(source, byteOffset, lineEnd);
                caretPrefix.push_back(' ');
                ++terminalColumn;
                index += length;
                ++sourceScalarsSeen;
            }
        }
        std::cerr << "  " << caretPrefix << "^\n";
    }
}

int main(int argc, char** argv)
{
    Options options;

    if (!parseArguments(argc, argv, options)) {
        if (psi::hadErrors()) {
            return 2;
        }
        return 0;
    }

    // Keep stderr machine-readable when JSON diagnostics are requested. The
    // diagnostic stream contains diagnostics only; informational logs stay off.
    psi::setVerbose(options.verbose && options.diagnosticFormat != "json");

    if (options.showVersion) {
        printVersion();
        return 0;
    }

    if (options.showHelp) {
        printUsage(std::cout);
        return 0;
    }

    if (options.jit && (options.outputPathSpecified || options.outputModeSpecified)) {
        psi::logError("--jit executes the program and cannot be combined with -o, --compile, or --emit-llvm");
        return 2;
    }
    if (options.entrySpecified && !options.jit) {
        psi::logError("--entry is only valid with --jit");
        return 2;
    }

    if (options.input.empty()) {
        psi::logError("no input files");
        psi::logNote("use 'psic --help' for usage information");
        return 2;
    }

    std::string source;
    if (!readSource(options.input, source)) {
        return 1;
    }

    if (options.output.empty()) {
        options.output = defaultOutputPath(options.input, options.emitIR);
    }

    if (options.jit) {
        psic::JitOptions jitOptions;
        jitOptions.compile.architecture = options.arch;
        jitOptions.compile.operatingSystem = options.os;
        jitOptions.compile.targetTriple = options.targetTriple;
        jitOptions.compile.targetCPU = options.targetCPU;
        jitOptions.compile.targetFeatures = options.targetFeatures;
        jitOptions.compile.optimization = options.opt;
        jitOptions.compile.sourceName = options.input == "-" ? "<stdin>" : options.input;
        jitOptions.entry = options.entry;
        auto result = psic::execute(source, jitOptions);
        psi::logInfo("Target profile: " + result.targetTriple);
        logTargetSubtarget(options);
        printDiagnostics(result.diagnostics, source,
            options.input == "-" ? "<stdin>" : options.input, options.diagnosticFormat);
        if (!result.success) return 1;
        return result.exitCode;
    }

    const std::string name = options.input == "-" ? "stdin" : options.input;

    try {
        psic::CompileOptions compileOptions;
        compileOptions.moduleName = name;
        compileOptions.sourceName = options.input == "-" ? "<stdin>" : options.input;
        compileOptions.architecture = options.arch;
        compileOptions.operatingSystem = options.os;
        compileOptions.targetTriple = options.targetTriple;
        compileOptions.targetCPU = options.targetCPU;
        compileOptions.targetFeatures = options.targetFeatures;
        compileOptions.optimization = options.opt;
        compileOptions.output = options.emitIR ? psic::OutputKind::LLVMIR : psic::OutputKind::Object;
        psi::logInfo("Compiling " + name + "...");
        auto result = psic::compile(source, compileOptions);
        psi::logInfo("Target profile: " + result.targetTriple);
        logTargetSubtarget(options);
        printDiagnostics(result.diagnostics, source,
            options.input == "-" ? "<stdin>" : options.input, options.diagnosticFormat);
        if (!result.success) return 1;
        const bool outputWritten = options.emitIR
            ? writeIR(result.ir, options.output)
            : writeObject(result.object, options.output);
        if (!outputWritten) return 1;
        if (options.output != "-")
            std::cout << "psic: compiled " << options.input << " -> " << options.output << "\n";
        return 0;
    } catch (const std::exception& error) {
        psi::logError(error.what());
        return 1;
    }
}
