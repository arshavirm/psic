#include <psic/c_api.h>

#include <string.h>

int main(void)
{
    const char source[] = "func i32 installed { ret 17; }";
    psic_compile_options options = {0};
    options.struct_size = (uint32_t)sizeof(options);
    psic_compile_result *result = psic_compile_source(source, strlen(source), &options);
    if (!result) return 1;
    size_t length = 0;
    const char *ir = psic_compile_result_ir(result, &length);
    int success = psic_compile_result_success(result) && ir && length
        && strstr(ir, "ret i32 17") != 0;
    psic_compile_result_destroy(result);
    return success ? 0 : 2;
}
