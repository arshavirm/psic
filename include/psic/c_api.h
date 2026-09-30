#ifndef PSIC_C_API_H
#define PSIC_C_API_H

#include <stddef.h>
#include <stdint.h>
#include <psic/export.hpp>

#ifdef __cplusplus
extern "C" {
#endif

/* Increment when an incompatible C ABI change is made. */
#define PSIC_C_API_VERSION 1u

/* Runtime counterpart to PSIC_C_API_VERSION for dynamically loaded libraries. */
PSIC_EXPORT uint32_t psic_c_api_version(void);

typedef enum psic_optimization_level {
    PSIC_OPTIMIZATION_DEFAULT = 0,
    PSIC_OPTIMIZATION_O0 = 1,
    PSIC_OPTIMIZATION_O1 = 2,
    PSIC_OPTIMIZATION_O2 = 3,
    PSIC_OPTIMIZATION_O3 = 4,
    PSIC_OPTIMIZATION_OS = 5,
    PSIC_OPTIMIZATION_OZ = 6
} psic_optimization_level;

typedef enum psic_output_kind {
    PSIC_OUTPUT_LLVM_IR = 0,
    PSIC_OUTPUT_OBJECT = 1
} psic_output_kind;

/* Zero-initialize this structure. Set struct_size to sizeof the structure used
 * by the caller; fields not present in an older structure use defaults. A zero
 * struct_size means the caller passes the current full structure. Future fields
 * are appended, and larger sizes are accepted for forward compatibility. */
typedef struct psic_compile_options {
    uint32_t struct_size;
    const char *module_name;
    const char *source_name;
    const char *architecture;
    const char *operating_system;
    const char *target_triple;
    /* Values from psic_optimization_level; zero selects the default (O2). */
    uint32_t optimization_level;
    /* Values from psic_output_kind. */
    uint32_t output_kind;
    /* Optional LLVM subtarget selector; empty CPU uses the generic CPU. */
    const char *target_cpu;
    /* Comma-separated LLVM feature overrides, for example "+avx2,-avx512f". */
    const char *target_features;
} psic_compile_options;

typedef struct psic_compile_result psic_compile_result;
typedef struct psic_jit_result psic_jit_result;

typedef struct psic_function_symbol {
    const char *name;
    void *address;
} psic_function_symbol;

/* Versioned JIT request. compile_options and symbols are read only during the
 * call; all strings and function addresses must remain valid until it returns. */
typedef struct psic_jit_options {
    uint32_t struct_size;
    const psic_compile_options *compile_options;
    const char *entry;
    const psic_function_symbol *external_functions;
    size_t external_function_count;
} psic_jit_options;

typedef enum psic_diagnostic_severity {
    PSIC_DIAGNOSTIC_ERROR = 0,
    PSIC_DIAGNOSTIC_WARNING = 1,
    PSIC_DIAGNOSTIC_NOTE = 2
} psic_diagnostic_severity;

/* Returns the most recent wrapper-level argument/setup error on this thread,
 * or null when the last compile/JIT call returned a result. The borrowed text
 * remains valid until the next compile/JIT call on the same thread. Language
 * diagnostics are returned through the result objects instead. */
PSIC_EXPORT const char *psic_last_error_message(void);

/* source may contain embedded NUL bytes. A null source is valid only when
 * source_length is zero. options may be null to use the default configuration.
 * Returns null only when the result cannot be allocated or arguments are
 * invalid. Compilation failures return a result with success() == 0 and
 * diagnostics available below. No exception crosses this C ABI. */
PSIC_EXPORT psic_compile_result *psic_compile_source(
    const char *source, size_t source_length, const psic_compile_options *options);

PSIC_EXPORT void psic_compile_result_destroy(psic_compile_result *result);
PSIC_EXPORT int psic_compile_result_success(const psic_compile_result *result);
/* Borrowed normalized target triple, valid until result_destroy(); null for a null result. */
PSIC_EXPORT const char *psic_compile_result_target_triple(const psic_compile_result *result);

/* Returned buffers are borrowed and remain valid until result_destroy(). */
PSIC_EXPORT const char *psic_compile_result_ir(
    const psic_compile_result *result, size_t *length);
PSIC_EXPORT const uint8_t *psic_compile_result_object(
    const psic_compile_result *result, size_t *length);

PSIC_EXPORT size_t psic_compile_result_diagnostic_count(
    const psic_compile_result *result);
/* Diagnostic strings are borrowed and remain valid until result_destroy().
 * An invalid index returns null for strings, error severity for severity, and
 * zero for coordinates. */
PSIC_EXPORT psic_diagnostic_severity psic_compile_result_diagnostic_severity(
    const psic_compile_result *result, size_t index);
PSIC_EXPORT const char *psic_compile_result_diagnostic_message(
    const psic_compile_result *result, size_t index);
PSIC_EXPORT const char *psic_compile_result_diagnostic_source_name(
    const psic_compile_result *result, size_t index);
PSIC_EXPORT size_t psic_compile_result_diagnostic_line(
    const psic_compile_result *result, size_t index);
PSIC_EXPORT size_t psic_compile_result_diagnostic_column(
    const psic_compile_result *result, size_t index);
/* Borrowed exact line bytes, valid until result_destroy(); length excludes no
 * bytes (including embedded NUL). Returns null when the line is unavailable. */
PSIC_EXPORT const char *psic_compile_result_diagnostic_source_line(
    const psic_compile_result *result, size_t index, size_t *length);

/* Compile and execute a native no-argument entry returning void or i32.
 * Cross-target requests fail with diagnostics. Returns null for invalid
 * arguments or allocation failures; execution/compiler failures return a JIT
 * result with success() == 0. No exception crosses this C ABI. */
PSIC_EXPORT psic_jit_result *psic_execute_source(
    const char *source, size_t source_length, const psic_jit_options *options);
PSIC_EXPORT void psic_jit_result_destroy(psic_jit_result *result);
PSIC_EXPORT int psic_jit_result_success(const psic_jit_result *result);
PSIC_EXPORT int32_t psic_jit_result_exit_code(const psic_jit_result *result);
/* Borrowed normalized native target triple, valid until result_destroy(); null for a null result. */
PSIC_EXPORT const char *psic_jit_result_target_triple(const psic_jit_result *result);
PSIC_EXPORT size_t psic_jit_result_diagnostic_count(const psic_jit_result *result);
PSIC_EXPORT psic_diagnostic_severity psic_jit_result_diagnostic_severity(
    const psic_jit_result *result, size_t index);
PSIC_EXPORT const char *psic_jit_result_diagnostic_message(
    const psic_jit_result *result, size_t index);
PSIC_EXPORT const char *psic_jit_result_diagnostic_source_name(
    const psic_jit_result *result, size_t index);
PSIC_EXPORT size_t psic_jit_result_diagnostic_line(
    const psic_jit_result *result, size_t index);
PSIC_EXPORT size_t psic_jit_result_diagnostic_column(
    const psic_jit_result *result, size_t index);
/* Borrowed exact line bytes, valid until result_destroy(); length excludes no
 * bytes (including embedded NUL). Returns null when the line is unavailable. */
PSIC_EXPORT const char *psic_jit_result_diagnostic_source_line(
    const psic_jit_result *result, size_t index, size_t *length);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSIC_C_API_H */
