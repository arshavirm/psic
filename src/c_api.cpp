#include <psic/c_api.h>
#include <psic/compiler.hpp>

#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>

namespace {
thread_local std::string lastErrorMessage;

void setLastErrorMessage(const char *message) noexcept
{
    try {
        lastErrorMessage = message ? message : "unknown C API failure";
    } catch (...) {
        lastErrorMessage.clear();
    }
}

void clearLastErrorMessage() noexcept
{
    lastErrorMessage.clear();
}
} // namespace

struct psic_compile_result {
    psic::CompileResult value;
};

struct psic_jit_result {
    psic::JitResult value;
    std::shared_ptr<psic::JitModule> module;
};

namespace {

psic::CompileOptions convertOptions(const psic_compile_options *options)
{
    psic::CompileOptions converted;
    if (!options) return converted;

    if (options->struct_size != 0 && options->struct_size < sizeof(options->struct_size))
        throw std::invalid_argument("psic_compile_options.struct_size is smaller than its header");

    // A zero size means the caller uses the current full structure. For a
    // versioned structure, do not read a field unless that field is present;
    // this lets an older embedding client run against a newer library.
    const size_t size = options->struct_size == 0
        ? sizeof(*options) : static_cast<size_t>(options->struct_size);
    const auto hasField = [size](size_t offset, size_t fieldSize) {
        return offset <= size && fieldSize <= size - offset;
    };

    if (hasField(offsetof(psic_compile_options, module_name), sizeof(options->module_name))
        && options->module_name) converted.moduleName = options->module_name;
    if (hasField(offsetof(psic_compile_options, source_name), sizeof(options->source_name))
        && options->source_name) converted.sourceName = options->source_name;
    if (hasField(offsetof(psic_compile_options, architecture), sizeof(options->architecture))
        && options->architecture) converted.architecture = options->architecture;
    if (hasField(offsetof(psic_compile_options, operating_system), sizeof(options->operating_system))
        && options->operating_system) converted.operatingSystem = options->operating_system;
    if (hasField(offsetof(psic_compile_options, target_triple), sizeof(options->target_triple))
        && options->target_triple) converted.targetTriple = options->target_triple;
    if (hasField(offsetof(psic_compile_options, target_cpu), sizeof(options->target_cpu))
        && options->target_cpu) converted.targetCPU = options->target_cpu;
    if (hasField(offsetof(psic_compile_options, target_features), sizeof(options->target_features))
        && options->target_features) converted.targetFeatures = options->target_features;

    if (hasField(offsetof(psic_compile_options, optimization_level), sizeof(options->optimization_level))) {
        switch (options->optimization_level) {
        case PSIC_OPTIMIZATION_DEFAULT: break;
        case PSIC_OPTIMIZATION_O0: converted.optimization = psic::OptimizationLevel::O0; break;
        case PSIC_OPTIMIZATION_O1: converted.optimization = psic::OptimizationLevel::O1; break;
        case PSIC_OPTIMIZATION_O2: converted.optimization = psic::OptimizationLevel::O2; break;
        case PSIC_OPTIMIZATION_O3: converted.optimization = psic::OptimizationLevel::O3; break;
        case PSIC_OPTIMIZATION_OS: converted.optimization = psic::OptimizationLevel::Os; break;
        case PSIC_OPTIMIZATION_OZ: converted.optimization = psic::OptimizationLevel::Oz; break;
        default:
            converted.optimization = static_cast<psic::OptimizationLevel>(-1);
            break;
        }
    }

    if (hasField(offsetof(psic_compile_options, output_kind), sizeof(options->output_kind))) {
        switch (options->output_kind) {
        case PSIC_OUTPUT_LLVM_IR: break;
        case PSIC_OUTPUT_OBJECT: converted.output = psic::OutputKind::Object; break;
        default:
            converted.output = static_cast<psic::OutputKind>(-1);
            break;
        }
    }
    return converted;
}

const psic::Diagnostic *getDiagnostic(const psic_compile_result *result, size_t index)
{
    if (!result || index >= result->value.diagnostics.size()) return nullptr;
    return &result->value.diagnostics[index];
}

const psic::Diagnostic *getDiagnostic(const psic_jit_result *result, size_t index)
{
    if (!result || index >= result->value.diagnostics.size()) return nullptr;
    return &result->value.diagnostics[index];
}

psic::JitOptions convertJitOptions(const psic_jit_options *options)
{
    psic::JitOptions converted;
    if (!options) return converted;
    if (options->struct_size != 0 && options->struct_size < sizeof(options->struct_size))
        throw std::invalid_argument("psic_jit_options.struct_size is smaller than its header");

    const size_t size = options->struct_size == 0
        ? sizeof(*options) : static_cast<size_t>(options->struct_size);
    const auto hasField = [size](size_t offset, size_t fieldSize) {
        return offset <= size && fieldSize <= size - offset;
    };
    if (hasField(offsetof(psic_jit_options, compile_options), sizeof(options->compile_options)))
        converted.compile = convertOptions(options->compile_options);
    if (hasField(offsetof(psic_jit_options, entry), sizeof(options->entry)) && options->entry)
        converted.entry = options->entry;

    const bool hasSymbols = hasField(offsetof(psic_jit_options, external_functions),
        sizeof(options->external_functions));
    const bool hasCount = hasField(offsetof(psic_jit_options, external_function_count),
        sizeof(options->external_function_count));
    if (hasCount && options->external_function_count != 0) {
        if (!hasSymbols || !options->external_functions
            || options->external_function_count > converted.externalFunctions.max_size())
            throw std::invalid_argument("invalid external function mapping array");
        converted.externalFunctions.reserve(options->external_function_count);
        for (size_t index = 0; index < options->external_function_count; ++index) {
            const psic_function_symbol& symbol = options->external_functions[index];
            if (!symbol.name)
                throw std::invalid_argument("external function mapping has a null name");
            converted.externalFunctions.push_back({symbol.name, symbol.address});
        }
    }
    return converted;
}

} // namespace

extern "C" {

uint32_t psic_c_api_version(void)
{
    return PSIC_C_API_VERSION;
}

const char *psic_last_error_message(void)
{
    return lastErrorMessage.empty() ? nullptr : lastErrorMessage.c_str();
}

psic_compile_result *psic_compile_source(const char *source, size_t source_length,
    const psic_compile_options *options)
{
    clearLastErrorMessage();
    if ((!source && source_length != 0)
        || source_length > std::string().max_size()) {
        setLastErrorMessage("invalid source pointer or source length");
        return nullptr;
    }

    try {
        auto result = std::make_unique<psic_compile_result>();
        const std::string_view sourceView(source ? source : "", source_length);
        result->value = psic::compile(sourceView, convertOptions(options));
        return result.release();
    } catch (const std::exception& error) {
        setLastErrorMessage(error.what());
        return nullptr;
    } catch (...) {
        setLastErrorMessage("unknown compile API failure");
        return nullptr;
    }
}

void psic_compile_result_destroy(psic_compile_result *result)
{
    delete result;
}

int psic_compile_result_success(const psic_compile_result *result)
{
    return result && result->value.success ? 1 : 0;
}

const char *psic_compile_result_target_triple(const psic_compile_result *result)
{
    return result ? result->value.targetTriple.c_str() : nullptr;
}

const char *psic_compile_result_ir(const psic_compile_result *result, size_t *length)
{
    if (length) *length = 0;
    if (!result || result->value.ir.empty()) return nullptr;
    if (length) *length = result->value.ir.size();
    return result->value.ir.data();
}

const uint8_t *psic_compile_result_object(const psic_compile_result *result, size_t *length)
{
    if (length) *length = 0;
    if (!result || result->value.object.empty()) return nullptr;
    if (length) *length = result->value.object.size();
    return result->value.object.data();
}

size_t psic_compile_result_diagnostic_count(const psic_compile_result *result)
{
    return result ? result->value.diagnostics.size() : 0;
}

psic_diagnostic_severity psic_compile_result_diagnostic_severity(
    const psic_compile_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    if (!diagnostic) return PSIC_DIAGNOSTIC_ERROR;
    return static_cast<psic_diagnostic_severity>(diagnostic->severity);
}

const char *psic_compile_result_diagnostic_message(const psic_compile_result *result,
    size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->message.c_str() : nullptr;
}

const char *psic_compile_result_diagnostic_source_name(
    const psic_compile_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->sourceName.c_str() : nullptr;
}

size_t psic_compile_result_diagnostic_line(const psic_compile_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->line : 0;
}

size_t psic_compile_result_diagnostic_column(const psic_compile_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->column : 0;
}

const char *psic_compile_result_diagnostic_source_line(
    const psic_compile_result *result, size_t index, size_t *length)
{
    if (length) *length = 0;
    const auto *diagnostic = getDiagnostic(result, index);
    if (!diagnostic || !diagnostic->hasSourceLine) return nullptr;
    if (length) *length = diagnostic->sourceLine.size();
    return diagnostic->sourceLine.data();
}

psic_jit_result *psic_execute_source(const char *source, size_t source_length,
    const psic_jit_options *options)
{
    clearLastErrorMessage();
    if ((!source && source_length != 0)
        || source_length > std::string().max_size()) {
        setLastErrorMessage("invalid source pointer or source length");
        return nullptr;
    }
    try {
        auto result = std::make_unique<psic_jit_result>();
        const std::string_view sourceView(source ? source : "", source_length);
        result->value = psic::execute(sourceView, convertJitOptions(options));
        return result.release();
    } catch (const std::exception& error) {
        setLastErrorMessage(error.what());
        return nullptr;
    } catch (...) {
        setLastErrorMessage("unknown JIT API failure");
        return nullptr;
    }
}

void psic_jit_result_destroy(psic_jit_result *result)
{
    delete result;
}

psic_jit_result *psic_prepare_jit_source(const char *source, size_t source_length,
    const psic_jit_options *options)
{
    clearLastErrorMessage();
    if ((!source && source_length != 0) || source_length > std::string().max_size()) {
        setLastErrorMessage("invalid source pointer or source length");
        return nullptr;
    }
    try {
        auto result = std::make_unique<psic_jit_result>();
        auto prepared = psic::prepareJit(std::string_view(source ? source : "", source_length),
            convertJitOptions(options));
        result->module = std::move(prepared.module);
        result->value = std::move(prepared);
        return result.release();
    } catch (const std::exception& error) {
        setLastErrorMessage(error.what());
        return nullptr;
    } catch (...) {
        setLastErrorMessage("unknown JIT API failure");
        return nullptr;
    }
}

uintptr_t psic_jit_result_function_address(const psic_jit_result *result, const char *name)
{
    if (!result || !result->value.success || !result->module || !name) return 0;
    try {
        return result->module->functionAddress(name);
    } catch (const std::exception& error) {
        setLastErrorMessage(error.what());
        return 0;
    } catch (...) {
        setLastErrorMessage("unknown JIT lookup failure");
        return 0;
    }
}

int psic_jit_result_success(const psic_jit_result *result)
{
    return result && result->value.success ? 1 : 0;
}

int32_t psic_jit_result_exit_code(const psic_jit_result *result)
{
    return result ? result->value.exitCode : 0;
}

const char *psic_jit_result_target_triple(const psic_jit_result *result)
{
    return result ? result->value.targetTriple.c_str() : nullptr;
}

size_t psic_jit_result_diagnostic_count(const psic_jit_result *result)
{
    return result ? result->value.diagnostics.size() : 0;
}

psic_diagnostic_severity psic_jit_result_diagnostic_severity(
    const psic_jit_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    if (!diagnostic) return PSIC_DIAGNOSTIC_ERROR;
    return static_cast<psic_diagnostic_severity>(diagnostic->severity);
}

const char *psic_jit_result_diagnostic_message(const psic_jit_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->message.c_str() : nullptr;
}

const char *psic_jit_result_diagnostic_source_name(
    const psic_jit_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->sourceName.c_str() : nullptr;
}

size_t psic_jit_result_diagnostic_line(const psic_jit_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->line : 0;
}

size_t psic_jit_result_diagnostic_column(const psic_jit_result *result, size_t index)
{
    const auto *diagnostic = getDiagnostic(result, index);
    return diagnostic ? diagnostic->column : 0;
}

const char *psic_jit_result_diagnostic_source_line(
    const psic_jit_result *result, size_t index, size_t *length)
{
    if (length) *length = 0;
    const auto *diagnostic = getDiagnostic(result, index);
    if (!diagnostic || !diagnostic->hasSourceLine) return nullptr;
    if (length) *length = diagnostic->sourceLine.size();
    return diagnostic->sourceLine.data();
}

} // extern "C"
