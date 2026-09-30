#include <psic/c_api.h>

#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *valid_source = "func i32 answer { ret 42; }";
    psic_compile_options options = {0};
    options.struct_size = (uint32_t)sizeof(options);
    options.source_name = "embedded/generated.psi";

    psic_compile_result *result = psic_compile_source(
        valid_source, strlen(valid_source), &options);
    if (!result || !psic_compile_result_success(result)) {
        psic_compile_result_destroy(result);
        return 1;
    }

    size_t ir_length = 0;
    const char *ir = psic_compile_result_ir(result, &ir_length);
    if (!ir || ir_length == 0 || !strstr(ir, "ret i32 42")) {
        psic_compile_result_destroy(result);
        return 2;
    }
    psic_compile_result_destroy(result);

    psic_compile_options jit_compile_options = {0};
    jit_compile_options.struct_size = (uint32_t)sizeof(jit_compile_options);
    jit_compile_options.output_kind = 1;
    psic_jit_options jit_options = {0};
    jit_options.struct_size = (uint32_t)sizeof(jit_options);
    jit_options.compile_options = &jit_compile_options;
    const char *jit_source = "entry main { ret; }";
    psic_jit_result *jit_result = psic_execute_source(
        jit_source, strlen(jit_source), &jit_options);
    if (!jit_result || psic_jit_result_success(jit_result)
        || psic_jit_result_diagnostic_count(jit_result) == 0
        || !strstr(psic_jit_result_diagnostic_message(jit_result, 0),
            "cannot request object-file output")) {
        psic_jit_result_destroy(jit_result);
        return 15;
    }
    psic_jit_result_destroy(jit_result);

    const char *invalid_source = "entry main {\n  i32 value = add 1;\n}";
    result = psic_compile_source(invalid_source, strlen(invalid_source), &options);
    if (!result || psic_compile_result_success(result)
        || psic_compile_result_diagnostic_count(result) == 0) {
        psic_compile_result_destroy(result);
        return 3;
    }
    if (psic_compile_result_diagnostic_severity(result, 0) != PSIC_DIAGNOSTIC_ERROR
        || psic_compile_result_diagnostic_line(result, 0) != 2
        || psic_compile_result_diagnostic_column(result, 0) != 3
        || strcmp(psic_compile_result_diagnostic_source_name(result, 0),
            "embedded/generated.psi") != 0) {
        psic_compile_result_destroy(result);
        return 4;
    }
    psic_compile_result_destroy(result);

    /* Simulate an older client whose structure ends before source_name.
     * The library must not read fields beyond the caller-provided size. */
    const size_t legacy_size = offsetof(psic_compile_options, source_name);
    psic_compile_options *legacy = (psic_compile_options *)malloc(legacy_size);
    if (!legacy)
        return 9;
    memset(legacy, 0, legacy_size);
    legacy->struct_size = (uint32_t)legacy_size;
    legacy->module_name = "legacy-client";
    result = psic_compile_source(valid_source, strlen(valid_source), legacy);
    free(legacy);
    if (!result || !psic_compile_result_success(result)) {
        psic_compile_result_destroy(result);
        return 10;
    }
    psic_compile_result_destroy(result);

    options.output_kind = 1;
    result = psic_compile_source(valid_source, strlen(valid_source), &options);
    if (!result || !psic_compile_result_success(result)) {
        psic_compile_result_destroy(result);
        return 5;
    }
    size_t object_length = 0;
    const uint8_t *object = psic_compile_result_object(result, &object_length);
    if (!object || object_length == 0 || psic_compile_result_ir(result, 0)) {
        psic_compile_result_destroy(result);
        return 6;
    }
    psic_compile_result_destroy(result);

    options.output_kind = 2;
    result = psic_compile_source(valid_source, strlen(valid_source), &options);
    if (!result || psic_compile_result_success(result)
        || psic_compile_result_diagnostic_count(result) == 0) {
        psic_compile_result_destroy(result);
        return 7;
    }
    psic_compile_result_destroy(result);

    options.output_kind = 0;
    options.struct_size = 1;
    if (psic_compile_source(valid_source, strlen(valid_source), &options) != 0)
        return 8;
    const char *api_error = psic_last_error_message();
    if (!api_error || !strstr(api_error, "struct_size"))
        return 11;

    options.struct_size = (uint32_t)sizeof(options);
    if (psic_compile_source(valid_source, strlen(valid_source), &options) == 0
        || psic_last_error_message() != 0)
        return 12;
    if (psic_compile_source(0, 1, &options) != 0)
        return 13;
    api_error = psic_last_error_message();
    if (!api_error || !strstr(api_error, "source pointer"))
        return 14;
    return 0;
}
