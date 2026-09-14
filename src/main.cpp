#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <psic/compiler.hpp>
#include "logging.hpp"

using OptLevel = psic::OptimizationLevel;

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
        << "  arm                       32-bit ARM\n"
        << "  wasm, wasm32, wasm64      WebAssembly (default OS: none)\n"
        << "  riscv32, riscv64          RISC-V\n"
        << "  ppc, ppc64, ppc64le       PowerPC\n"
        << "  mips, mipsel, mips64, mips64el\n"
        << "  loongarch64, s390x        LoongArch / SystemZ\n\n"
        << "Operating systems:\n"
        << "  wasi                      WebAssembly System Interface\n"
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
        psic::CompileOptions compileOptions;
        compileOptions.moduleName = name;
        compileOptions.architecture = options.arch;
        compileOptions.operatingSystem = options.os;
        compileOptions.targetTriple = options.targetTriple;
        compileOptions.optimization = options.opt;
        compileOptions.output = options.emitIR ? psic::OutputKind::LLVMIR : psic::OutputKind::Object;
        psi::logInfo("Compiling " + name + "...");
        auto result = psic::compile(source, compileOptions);
        for (const auto& diagnostic : result.diagnostics) {
            switch (diagnostic.severity) {
            case psic::DiagnosticSeverity::Error: psi::logError(diagnostic.message); break;
            case psic::DiagnosticSeverity::Warning: psi::logWarning(diagnostic.message); break;
            case psic::DiagnosticSeverity::Note: psi::logNote(diagnostic.message); break;
            }
        }
        if (!result.success) return 1;
        if (options.emitIR) {
            if (!writeIR(result.ir, options.output)) return 1;
        } else {
            if (options.output == "-") {
                psi::logError("object output to stdout is not supported; specify an output file");
                return 1;
            }
            std::ofstream output(options.output, std::ios::binary);
            if (!output) {
                psi::logError("cannot open output file '" + options.output + "'");
                return 1;
            }
            output.write(reinterpret_cast<const char*>(result.object.data()), result.object.size());
            output.close();
            if (!output) {
                psi::logError("error writing output file '" + options.output + "'");
                return 1;
            }
        }
        if (options.output != "-")
            std::cout << "psic: compiled " << options.input << " -> " << options.output << "\n";
        return 0;
    } catch (const std::exception& error) {
        psi::logError(error.what());
        return 1;
    }
}
