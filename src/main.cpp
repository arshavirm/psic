#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast.hpp"
#include "codegen.hpp"
#include "lexer.hpp"
#include "logging.hpp"
#include "parser.hpp"

const std::string BANNER = R"(
█████ █████ █████
█   █ █       █  
█████ █████   █  
█         █   █  
█     █████ █████ Program System Interface
)";

static constexpr const char* VERSION = "psic 0.1.0";

struct Options {
    std::string input;
    std::string output;
    std::string targetTriple;
    std::string arch;
    std::string os;
    OptLevel opt;

    bool compileOnly = false;
    bool emitIR = false;
    bool showHelp = false;
    bool showVersion = false;
    bool verbose = false;
};

static void printUsage(std::ostream& out)
{
    out << BANNER << "\n";

    out << "Usage: psic [options] <input>\n\n"
        << "Compile a PSI source file into an object file or LLVM IR.\n\n"
        << "Options:\n"
        << "  -o, --output <file>       Write output to <file>\n"
        << "  -c, --compile             Compile to an object file (default)\n"
        << "  --emit-llvm               Write LLVM IR text instead of an object file\n"
        << "  -target <triple>          Set target triple\n"
        << "  -march, --arch <arch>     Set target architecture\n"
        << "  -mos, --os <os>           Set target OS\n"
        << "  -v, --verbose             Print compilation details\n"
        << "  -h, --help                Show this help message\n"
        << "      --version             Show compiler version\n\n"
        << "Architectures:\n"
        << "  x86                       32-bit x86\n"
        << "  x86_64, x86-64, amd64     64-bit x86\n"
        << "  aarch64                   64-bit ARM\n"
        << "  arm                       32-bit ARM\n\n"
        << "Operating systems:\n"
        << "  linux                     Linux (default)\n"
        << "  darwin, macos             macOS\n"
        << "  windows, win32            Windows\n"
        << "  none, freestanding        Bare metal / no OS\n\n"
        << "Examples:\n"
        << "  psic hello.psi\n"
        << "  psic hello.psi -o hello.o\n"
        << "  psic -c hello.psi -o build/hello.o\n"
        << "  psic --target x86_64-pc-linux-gnu hello.psi -o hello.o\n"
        << "  psic -march x86_64 hello.psi -o hello.o\n"
        << "  psic -march aarch64 -mos darwin hello.psi -o hello.o\n"
        << "  psic --emit-llvm hello.psi -o hello.ll\n";
}

static void printVersion()
{
    std::cout << VERSION << '\n';
}

static bool isOption(const std::string& arg)
{
    return arg.size() > 1 && arg[0] == '-';
}

static bool requireValue(int argc, char** argv, int& i, const std::string& option,
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

static bool parseArguments(int argc, char** argv, Options& options)
{
    bool endOfOptions = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (!endOfOptions && arg == "--") {
            endOfOptions = true;
            continue;
        }

        if (!endOfOptions && (arg == "-h" || arg == "--help")) {
            options.showHelp = true;
            continue;
        }

        if (!endOfOptions && arg == "--version") {
            options.showVersion = true;
            continue;
        }

        if (!endOfOptions && (arg == "-v" || arg == "--verbose")) {
            options.verbose = true;
            continue;
        }

        if (!endOfOptions && (arg == "-c" || arg == "--compile")) {
            options.compileOnly = true;
            continue;
        }

        if (!endOfOptions && arg == "--emit-llvm") {
            options.emitIR = true;
            continue;
        }

        if (!endOfOptions && (arg == "-o" || arg == "--output")) {
            if (!requireValue(argc, argv, i, arg, options.output))
                return false;
            continue;
        }

        if (!endOfOptions && (arg == "-target" || arg == "--target")) {
            if (!requireValue(argc, argv, i, arg, options.targetTriple))
                return false;
            continue;
        }

        if (!endOfOptions && (arg == "-march" || arg == "--arch")) {
            if (!requireValue(argc, argv, i, arg, options.arch))
                return false;
            continue;
        }

        if (!endOfOptions && (arg == "-mos" || arg == "--os")) {
            if (!requireValue(argc, argv, i, arg, options.os))
                return false;
            continue;
        }

        if (!endOfOptions && (arg == "-O0")) {
            options.opt = OptLevel::O0;
            continue;
        }

        if (!endOfOptions && (arg == "-O1")) {
            options.opt = OptLevel::O1;
            continue;
        }

        if (!endOfOptions && (arg == "-O2")) {
            options.opt = OptLevel::O2;
            continue;
        }

        if (!endOfOptions && (arg == "-O3")) {
            options.opt = OptLevel::O3;
            continue;
        }

        if (!endOfOptions && (arg == "-Os")) {
            options.opt = OptLevel::Os;
            continue;
        }

        if (!endOfOptions && (arg == "-Oz")) {
            options.opt = OptLevel::Oz;
            continue;
        }

        if (!endOfOptions && arg.rfind("--target=", 0) == 0) {
            options.targetTriple = arg.substr(9);
            if (options.targetTriple.empty()) {
                psi::logError("'--target=' requires an argument");
                return false;
            }
            continue;
        }

        if (!endOfOptions && arg.rfind("--arch=", 0) == 0) {
            options.arch = arg.substr(7);
            if (options.arch.empty()) {
                psi::logError("'--arch=' requires an argument");
                return false;
            }
            continue;
        }

        if (!endOfOptions && arg.rfind("--os=", 0) == 0) {
            options.os = arg.substr(5);
            if (options.os.empty()) {
                psi::logError("'--os=' requires an argument");
                return false;
            }
            continue;
        }

        if (!endOfOptions && arg.rfind("--output=", 0) == 0) {
            options.output = arg.substr(9);
            if (options.output.empty()) {
                psi::logError("'--output=' requires an argument");
                return false;
            }
            continue;
        }

        if (!endOfOptions && arg == "-") {
            if (!options.input.empty()) {
                psi::logError("multiple input files are not supported");
                return false;
            }
            options.input = "-";
            continue;
        }

        if (!endOfOptions && isOption(arg)) {
            psi::logError("unknown option '" + arg + "'");
            psi::logNote("use 'psic --help' for usage information");
            return false;
        }

        if (!options.input.empty()) {
            psi::logError("multiple input files are not supported");
            return false;
        }

        options.input = arg;
    }

    return true;
}

Architecture pickArchitecture(const std::string& explicitArch,
    const std::string& targetTriple)
{
    if (explicitArch == "x86") {
        return Architecture::X86;
    }

    if (explicitArch == "x86_64" || explicitArch == "x86-64" || explicitArch == "amd64") {
        return Architecture::X86_64;
    }

    if (explicitArch == "aarch64") {
        return Architecture::AArch64;
    }

    if (explicitArch == "arm") {
        return Architecture::ARM;
    }

    if (!explicitArch.empty()) {
        throw std::runtime_error("unknown architecture '" + explicitArch + "'");
    }

    if (!targetTriple.empty()) {
        if (targetTriple.rfind("x86_64", 0) == 0 || targetTriple.rfind("amd64", 0) == 0) {
            return Architecture::X86_64;
        }

        if (targetTriple.rfind("i386", 0) == 0 || targetTriple.rfind("i486", 0) == 0 || targetTriple.rfind("i586", 0) == 0 || targetTriple.rfind("i686", 0) == 0 || targetTriple.rfind("x86-", 0) == 0) {
            return Architecture::X86;
        }

        if (targetTriple.rfind("aarch64", 0) == 0) {
            return Architecture::AArch64;
        }

        if (targetTriple.rfind("armv7", 0) == 0) {
            return Architecture::ARM;
        }

        throw std::runtime_error(
            "cannot determine architecture from target triple '" + targetTriple + "'");
    }

    return Architecture::X86_64;
}

OperatingSystem pickOperatingSystem(const std::string& explicitOs,
    const std::string& targetTriple)
{
    if (explicitOs == "linux") {
        return OperatingSystem::Linux;
    }

    if (explicitOs == "darwin" || explicitOs == "macos" || explicitOs == "macosx") {
        return OperatingSystem::Darwin;
    }

    if (explicitOs == "windows" || explicitOs == "win32") {
        return OperatingSystem::Windows;
    }

    if (explicitOs == "none" || explicitOs == "freestanding" || explicitOs == "bare-metal") {
        return OperatingSystem::FreeStanding;
    }

    if (!explicitOs.empty()) {
        throw std::runtime_error("unknown OS '" + explicitOs + "'");
    }

    if (!targetTriple.empty()) {
        if (targetTriple.find("linux") != std::string::npos) {
            return OperatingSystem::Linux;
        }
        if (targetTriple.find("apple") != std::string::npos || targetTriple.find("darwin") != std::string::npos || targetTriple.find("macos") != std::string::npos || targetTriple.find("ios") != std::string::npos) {
            return OperatingSystem::Darwin;
        }
        if (targetTriple.find("windows") != std::string::npos) {
            return OperatingSystem::Windows;
        }
        if (targetTriple.find("-none") != std::string::npos) {
            return OperatingSystem::FreeStanding;
        }

        throw std::runtime_error(
            "cannot determine OS from target triple '" + targetTriple + "' - pass -mos explicitly");
    }

    return OperatingSystem::Linux;
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
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        source = buffer.str();
        return true;
    }

    std::ifstream file(input);
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

int main(int argc, char** argv)
{
    Options options;
    options.opt = OptLevel::O2;

    if (!parseArguments(argc, argv, options)) {
        if (psi::hadErrors()) {
            return 2;
        }
        return 0;
    }

    psi::setVerbose(options.verbose);

    if (options.showVersion) {
        printVersion();
        return 0;
    }

    if (options.showHelp) {
        printUsage(std::cout);
        return 0;
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

    const std::string name = options.input == "-" ? "stdin" : options.input;

    try {
        psi::logInfo("Lexing...");
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();

        psi::logInfo("Parsing...");
        Parser parser(tokens);
        ProgramNode program = parser.parseProgram();

        Architecture arch = pickArchitecture(options.arch, options.targetTriple);
        OperatingSystem os = pickOperatingSystem(options.os, options.targetTriple);

        if (options.verbose) {
            psi::logInfo("Input:  " + options.input);
            psi::logInfo("Output: " + options.output);
            psi::logInfo("Target: " + options.targetTriple);
            psi::logInfo("Arch:   " + options.arch);
            psi::logInfo("OS:     " + options.os);
            psi::logInfo("Optimization: " + std::to_string(static_cast<int>(options.opt)));
            psi::logInfo("Generating IR...");
        }

        psi::logInfo("Compiling to IR...");
        std::string ir = compileProgram(name, program, arch, os);

        if (psi::hadErrors()) {
            psi::logError("compilation failed with " + std::to_string(psi::errorCount()) + " error(s)");
            return 1;
        }

        if (options.emitIR) {
            if (!writeIR(ir, options.output)) {
                return 1;
            }

            if (options.verbose) {
                psi::logInfo("Successfully wrote " + options.output);
            } else if (options.output != "-") {
                std::cout << "psic: emitted LLVM IR " << options.input << " -> " << options.output
                          << "\n";
            }
            return 0;
        }

        if (options.verbose) {
            psi::logInfo("Generating object file...");
        }

        std::string errorMessage;
        if (!compileToObjectFile(
                ir, options.targetTriple, options.output, options.opt, errorMessage)) {
            psi::logError("object file generation failed: " + errorMessage);
            return 1;
        }

        if (options.verbose) {
            psi::logInfo("Successfully wrote " + options.output);
        } else {
            std::cout << "psic: compiled " << options.input << " -> " << options.output << "\n";
        }

        return 0;
    } catch (const std::exception& error) {
        psi::logError(error.what());
        return 1;
    }
}
