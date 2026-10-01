#include <psic/compiler.hpp>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>

static std::int32_t hostIncrement(std::int32_t value)
{
    return value + 1;
}

static void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
static std::string diagnostics(const psic::CompileResult& result)
{
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.message + "\n";
    return text;
}
static std::string diagnostics(const psic::JitResult& result)
{
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.message + "\n";
    return text;
}
static void reject(const std::string& source, const std::string& expected)
{
    auto result = psic::compile(source);
    check(!result.success, "unexpected success: " + source);
    check(result.ir.empty() && result.object.empty(), "failed compilation returned output");
    check(diagnostics(result).find(expected) != std::string::npos,
        "missing diagnostic '" + expected + "': " + diagnostics(result));
    check(psic::compile("func i32 recover { ret 42; }").success, "compiler did not recover after failure");
}

int main()
{
    try {
        std::ostringstream captured;
        auto* previous = std::cerr.rdbuf(captured.rdbuf());
        struct Restore { std::streambuf* previous; ~Restore() { std::cerr.rdbuf(previous); } } restore{previous};
        psic::CompileOptions options;
        options.optimization = psic::OptimizationLevel::O0;
        auto first = psic::compile("func i32 sum i32 a i32 b { i32 result = add a b; ret result; }", options);
        check(first.success && first.ir.find("define i32 @sum") != std::string::npos, diagnostics(first));
        check(first.object.empty(), "IR compilation produced an object");
        psic::ModuleBuilder generated("embedded-module");
        generated.declaration("i32 embedded_global = 1;")
            .function("i32", "embedded_answer", "", "ret 42;")
            .entry("embedded_entry", "ret;");
        auto generatedResult = generated.compile(options);
        check(generated.source().find("func i32 embedded_answer") != std::string::npos
                && generatedResult.success
                && generatedResult.ir.find("define i32 @embedded_answer") != std::string::npos,
            "ModuleBuilder did not assemble and compile caller-provided PSI fragments: "
                + diagnostics(generatedResult));
        psic::CompileOptions namedSourceOptions = options;
        namedSourceOptions.sourceName = "generated/module.psi";
        auto locatedError = psic::compile(
            "entry main {\n  i32 result = add 1;\n}", namedSourceOptions);
        check(!locatedError.success && !locatedError.diagnostics.empty(),
            "invalid source unexpectedly compiled");
        const auto& location = locatedError.diagnostics.front();
        check(location.sourceName == "generated/module.psi" && location.line == 2
                && location.column == 3,
            "API diagnostic lost source name or coordinates");
        for (const std::string& lineEnding : {std::string("\r\n"), std::string("\r")}) {
            auto newlineDiagnostic = psic::compile(
                "entry main {" + lineEnding + "  i32 result = add 1;" + lineEnding + "}",
                namedSourceOptions);
            check(!newlineDiagnostic.success && !newlineDiagnostic.diagnostics.empty()
                    && newlineDiagnostic.diagnostics.front().line == 2
                    && newlineDiagnostic.diagnostics.front().column == 3,
                "source coordinates are incorrect for CR or CRLF line endings");
        }
        auto codegenLocatedError = psic::compile(
            "entry main {\n  #unknown_operation;\n}", namedSourceOptions);
        check(!codegenLocatedError.success && !codegenLocatedError.diagnostics.empty()
                && codegenLocatedError.diagnostics.front().line == 2,
            "code generation diagnostic lost its source location");
        auto afterTerminatorError = psic::compile(
            "entry main {\n  ret;\n  i32 unreachable = 1;\n}", namedSourceOptions);
        check(!afterTerminatorError.success && !afterTerminatorError.diagnostics.empty()
                && afterTerminatorError.diagnostics.front().line == 3
                && afterTerminatorError.diagnostics.front().column == 3
                && afterTerminatorError.diagnostics.front().hasSourceLine
                && afterTerminatorError.diagnostics.front().sourceLine == "  i32 unreachable = 1;",
            "instruction-after-terminator diagnostic lost its source line and coordinates");
        auto octal = psic::compile("func i32 octal { ret 0o777; }", options);
        check(octal.success && octal.ir.find("ret i32 511") != std::string::npos,
            "octal literal was not parsed: " + diagnostics(octal));
        auto upperOctal = psic::compile("func i32 octal { ret 0O10; }", options);
        check(upperOctal.success && upperOctal.ir.find("ret i32 8") != std::string::npos,
            "uppercase octal literal was not parsed: " + diagnostics(upperOctal));
        auto commentsAndSyntax = psic::compile(
            "/* first line\n second line */ struct Pair { i32 left; i32 right }\n"
            "func i32 value { i32* values = [0o1, 0O2, 3]; ret values[2]; }", options);
        check(commentsAndSyntax.success, "new literal/container syntax failed: " + diagnostics(commentsAndSyntax));
        auto underAlignedLocal = psic::compile(
            "func i32 alignment { i32:1 value = 7; ret value; }", options);
        check(underAlignedLocal.success
                && underAlignedLocal.ir.find("alloca i32, align 4") != std::string::npos,
            "local alignment suffix weakened the ABI-required alignment: "
                + diagnostics(underAlignedLocal));
        auto functionPointer = psic::compile(
            "func i32 invoke i32* fptr { i32 result = call fptr; ret result; }", options);
        check(functionPointer.success, "indirect function pointer call failed: " + diagnostics(functionPointer));
        auto portableSpecials = psic::compile(
            "func i32 specials i8* dst i8* src u32 size i32* cell { "
            "#memcpy dst src size; #memset dst 0 1; #cpu_relax; "
            "i32 expected = 0; i32 desired = 1; "
            "i32 old = #atomic_cas cell expected desired; u32 bits = #bswap 1; "
            "u32 count = #popcnt bits; u32 lead = #clz count; u32 trail = #ctz lead; "
            "#nop; ret old; }", options);
        check(portableSpecials.success, "new portable instructions failed: " + diagnostics(portableSpecials));
        auto systemAtomics = psic::compile(
            "func i32 atomics i32* pointer i32 value { i32 loaded = #atomicload pointer; "
            "#atomicstore pointer value; i32 old = #atomicadd pointer value; #fence; ret loaded; }");
        check(systemAtomics.success
                && systemAtomics.ir.find("load atomic i32") != std::string::npos
                && systemAtomics.ir.find("store atomic i32") != std::string::npos
                && systemAtomics.ir.find("syncscope(\"singlethread\")") == std::string::npos,
            "atomic loads/stores are not system-scope sequentially consistent operations: "
                + diagnostics(systemAtomics));
        auto atomicNull = psic::compile(
            "func i32 atomic_null { i32 value = #atomicload null; ret value; }");
        check(atomicNull.success && atomicNull.ir.find("@llvm.trap") != std::string::npos,
            "atomic null access did not lower to a language trap: " + diagnostics(atomicNull));
        auto jit = psic::execute("func i32 main { ret 23; }");
        std::string jitDiagnostics;
        for (const auto& diagnostic : jit.diagnostics) jitDiagnostics += diagnostic.message + "\n";
        check(jit.success && jit.exitCode == 23, "library JIT execution failed: " + jitDiagnostics);
        psic::JitOptions objectOutputJit;
        objectOutputJit.compile.output = psic::OutputKind::Object;
        auto rejectedObjectOutputJit = psic::execute("entry main { ret; }", objectOutputJit);
        check(!rejectedObjectOutputJit.success
                && !rejectedObjectOutputJit.diagnostics.empty()
                && rejectedObjectOutputJit.diagnostics.front().message.find(
                    "cannot request object-file output") != std::string::npos,
            "JIT silently ignored an object-output request");
        const std::string externalJitSource =
            "func i32 host_increment i32 value; "
            "func i32 main { i32 result = call host_increment 41; ret result; }";
        auto missingExternalJit = psic::execute(externalJitSource);
        check(!missingExternalJit.success && !missingExternalJit.diagnostics.empty()
                && missingExternalJit.diagnostics.front().message.find("no explicit host mapping")
                    != std::string::npos,
            "JIT accepted an external function without an explicit host mapping");
        psic::JitOptions mappedExternalJit;
        mappedExternalJit.externalFunctions.push_back({
            "host_increment", reinterpret_cast<void*>(&hostIncrement)});
        auto mappedJit = psic::execute(externalJitSource, mappedExternalJit);
        check(mappedJit.success && mappedJit.exitCode == 42,
            "JIT external function mapping failed: " + diagnostics(mappedJit));
        auto unsignedJitEntry = psic::execute("func u32 main { ret 7; }");
        check(!unsignedJitEntry.success && !unsignedJitEntry.diagnostics.empty()
                && unsignedJitEntry.diagnostics.front().message.find("return void or i32")
                    != std::string::npos,
            "JIT accepted a u32 entry because LLVM erased signedness");
        auto argumentJitEntry = psic::execute("func i32 main i32 argument { ret argument; }");
        check(!argumentJitEntry.success && !argumentJitEntry.diagnostics.empty()
                && argumentJitEntry.diagnostics.front().message.find("no arguments")
                    != std::string::npos,
            "JIT accepted an entry with arguments");
        psic::JitOptions crossTargetJit;
#if defined(__aarch64__) || defined(_M_ARM64)
        crossTargetJit.compile.targetTriple = "x86_64-unknown-linux-gnu";
#else
        crossTargetJit.compile.targetTriple = "aarch64-unknown-linux-gnu";
#endif
        auto rejectedCrossTarget = psic::execute("entry main { ret; }", crossTargetJit);
        check(!rejectedCrossTarget.success && !rejectedCrossTarget.diagnostics.empty()
                && rejectedCrossTarget.diagnostics.front().message.find("native target triple")
                    != std::string::npos,
            "JIT executed source compiled for a different target");
        psic::JitOptions unsupportedJitFeature;
        unsupportedJitFeature.compile.targetCPU = "generic";
        unsupportedJitFeature.compile.targetFeatures = "+psic_feature_not_present_on_any_host";
        auto rejectedJitFeature = psic::execute("entry main { ret; }", unsupportedJitFeature);
        check(!rejectedJitFeature.success && !rejectedJitFeature.diagnostics.empty()
                && rejectedJitFeature.diagnostics.front().message.find("not supported by the host")
                    != std::string::npos,
            "JIT accepted an instruction feature unavailable on the host: "
                + diagnostics(rejectedJitFeature));
        auto guardedCoreOps = psic::compile(
            "func i32 core i32 a i32 b { i32 quotient = div a b; i32 remainder = mod a b; "
            "i32 shifted = lsh a b; i32 sum = add quotient shifted; ret sum; }",
            options);
        check(guardedCoreOps.success, diagnostics(guardedCoreOps));
        check(guardedCoreOps.ir.find("@llvm.trap") != std::string::npos
                && guardedCoreOps.ir.find("icmp eq i32") != std::string::npos
                && guardedCoreOps.ir.find("-2147483648") != std::string::npos
                && guardedCoreOps.ir.find("icmp uge i32") != std::string::npos
                && guardedCoreOps.ir.find("sdiv i32") != std::string::npos
                && guardedCoreOps.ir.find("srem i32") != std::string::npos,
            "division, remainder, and shift edge behavior was not guarded");
        auto guardedVectorDivision = psic::compile(
            "func i32 vector_division i32x4 a i32x4 b { "
            "i32x4 quotient = #vdivi a b; i32x4 unsigned_quotient = #vdivu a b; "
            "ret 0; }", options);
        check(guardedVectorDivision.success, diagnostics(guardedVectorDivision));
        check(guardedVectorDivision.ir.find("vector.div.trap") != std::string::npos
                && guardedVectorDivision.ir.find("extractelement <4 x i1>") != std::string::npos
                && guardedVectorDivision.ir.find("sdiv <4 x i32>") != std::string::npos
                && guardedVectorDivision.ir.find("udiv <4 x i32>") != std::string::npos,
            "integer vector division lacks per-lane trap guards");
        auto second = psic::compile("func i32 sum { ret 7; }");
        check(second.success && second.ir.find("ret i32 7") != std::string::npos, "state leaked between compilations");
        auto escapedString = psic::compile(R"(func i8* text { ret "a\"b"; })");
        check(escapedString.success, "escaped quote ended the string early: " + diagnostics(escapedString));
        auto pointerCast = psic::compile("func i32* cast i8* p { i32* q = #ptrcast p; ret q; }");
        check(pointerCast.success, "explicit pointer cast failed: " + diagnostics(pointerCast));
        auto pointerOffset = psic::compile(
            "func i32* offset i32* pointer u64 index { i32* result = add pointer index; ret result; }");
        check(pointerOffset.success
                && pointerOffset.ir.find("getelementptr i32") != std::string::npos
                && pointerOffset.ir.find("ptrtoint ptr") == std::string::npos,
            "pointer addition did not preserve the target pointer representation: "
                + diagnostics(pointerOffset));
        auto numericCasts = psic::compile(
            "func i32 cast i32 value u8 byte { i8 low = #trunc value; "
            "i32 signedValue = #sext low; u32 unsignedValue = #zext byte; "
            "f32 lowFloat = #fptrunc 1.5; f64 wide = #fpext lowFloat; "
            "i32 result = add signedValue unsignedValue; ret result; }");
        check(numericCasts.success, "explicit numeric casts failed: " + diagnostics(numericCasts));
        reject("i8 narrowed = 258.75;",
            "floating-point literal requires a floating-point destination");
        reject("i64 signed_min = -9223372036854775808.0;",
            "floating-point literal requires a floating-point destination");
        auto minimumOctalLiteral = psic::compile(
            "const i64 signed_minimum = -0o1000000000000000000000;");
        check(minimumOctalLiteral.success,
            "the signed 64-bit minimum octal literal was rejected: "
                + diagnostics(minimumOctalLiteral));
        reject("const i64 out_of_range = 0o1000000000000000000000;",
            "integer literal is out of range");
        auto locatedGlobalConversionError = psic::compile(
            "i32 okay = 1;\ni64 too_large = 9223372036854775808.0;",
            namedSourceOptions);
        check(!locatedGlobalConversionError.success
                && !locatedGlobalConversionError.diagnostics.empty()
                && locatedGlobalConversionError.diagnostics.front().line == 2
                && locatedGlobalConversionError.diagnostics.front().column == 1
                && locatedGlobalConversionError.diagnostics.front().sourceName
                    == "generated/module.psi",
            "global conversion error lost its source line or name");
        auto locatedGlobalValidationError = psic::compile(
            "i32 okay = 1;\ni32* invalid = 12;", namedSourceOptions);
        check(!locatedGlobalValidationError.success
                && !locatedGlobalValidationError.diagnostics.empty()
                && locatedGlobalValidationError.diagnostics.front().line == 2
                && locatedGlobalValidationError.diagnostics.front().column == 1
                && locatedGlobalValidationError.diagnostics.front().sourceName
                    == "generated/module.psi",
            "top-level validation error lost its source line or name");
        auto checkedFloatCast = psic::compile(
            "func i32 checked f64 value { i32 result = #fptosi value; ret result; }");
        check(checkedFloatCast.success && checkedFloatCast.ir.find("@llvm.trap") != std::string::npos,
            "float-to-integer conversion is not range-checked: " + diagnostics(checkedFloatCast));
        auto pointerIntegerCast = psic::compile("func u64 address i8* p { u64 n = #ptrtoint p; ret n; }");
        check(pointerIntegerCast.success, "explicit pointer-to-integer cast failed: " + diagnostics(pointerIntegerCast));
        psic::CompileOptions wasmPointerOptions;
        wasmPointerOptions.architecture = "wasm32";
        auto wasmPointerIntegerCast = psic::compile("func u32 address i8* p { u32 n = #ptrtoint p; ret n; }", wasmPointerOptions);
        check(wasmPointerIntegerCast.success, "32-bit pointer conversion failed: " + diagnostics(wasmPointerIntegerCast));
        auto integerPointerCast = psic::compile("func i8* restore u64 n { i8* p = #inttoptr n; ret p; }");
        check(integerPointerCast.success, "explicit integer-to-pointer cast failed: " + diagnostics(integerPointerCast));
        auto nullPointer = psic::compile("func i32* empty { i32* p = null; ret p; }");
        check(nullPointer.success, "null pointer initialization failed: " + diagnostics(nullPointer));
        auto referenceTypes = psic::compile("func i32 reference { i32 x = 5; i32* p = ref x; i32** pp = ref p; i32 y = p[0]; ret y; }");
        check(referenceTypes.success, "reference pointer depth handling failed: " + diagnostics(referenceTypes));
        auto checkedView = psic::compile(
            "entry main { i32[] values = [4, 8, 15]; u64 index = %rax; "
            "i32[] alias = values; "
            "u64 length = #viewlen values; i32 item = #viewload values index; "
            "#viewstore alias index item; ret; }");
        check(checkedView.success && checkedView.ir.find("view.bounds.trap") != std::string::npos
                && checkedView.ir.find("@llvm.trap") != std::string::npos,
            "bounded views did not emit a runtime bounds trap: " + diagnostics(checkedView));
        psic::CompileOptions viewSliceOptions;
        viewSliceOptions.optimization = psic::OptimizationLevel::O0;
        auto checkedViewSlice = psic::compile(
            "entry main { i32[] values = [4, 8, 15]; u64 start = %rax; "
            "i32[] part = #viewslice values start 1; "
            "i32 first = #viewload part 0; ret; }", viewSliceOptions);
        check(checkedViewSlice.success
                && checkedViewSlice.ir.find("view.slice.trap") != std::string::npos
                && checkedViewSlice.ir.find("view.slice.data") != std::string::npos,
            "bounded view slicing lacks range checks or pointer-preserving lowering: "
                + diagnostics(checkedViewSlice));
        auto nullMemoryAccess = psic::compile(
            "func i32 read i32* pointer { i32 value = load pointer; ret value; } "
            "func void write i32* pointer { i32 value = 11; store pointer value; ret; }");
        check(nullMemoryAccess.success
                && nullMemoryAccess.ir.find("icmp eq ptr") != std::string::npos
                && nullMemoryAccess.ir.find("@llvm.trap") != std::string::npos,
            "optimized load/store lowering lacks a null-access trap: "
                + diagnostics(nullMemoryAccess));
        auto nullIndexedAccess = psic::compile(
            "func i32 indexed_read i32* pointer { ret pointer[0]; } "
            "func void indexed_write i32* pointer { pointer[0] = 1; ret; }");
        check(nullIndexedAccess.success
                && nullIndexedAccess.ir.find("null.access.trap") != std::string::npos
                && nullIndexedAccess.ir.find("@llvm.trap") != std::string::npos,
            "pointer-indexed reads and writes lack null-access traps: "
                + diagnostics(nullIndexedAccess));
        auto nullChainedAccess = psic::compile(
            "func i32 indexed_chain i32** pointer { ret pointer[0][0]; }");
        check(nullChainedAccess.success
                && nullChainedAccess.ir.find("icmp eq ptr") != std::string::npos
                && nullChainedAccess.ir.find("@llvm.trap") != std::string::npos,
            "chained pointer indexing lacks an intermediate null check: "
                + diagnostics(nullChainedAccess));
        auto nullAddressOnly = psic::compile(
            "func i32* indexed_address i32* pointer { i32* result = ref pointer[0]; ret result; }");
        check(nullAddressOnly.success && nullAddressOnly.ir.find("@llvm.trap") == std::string::npos,
            "ref of a final indexed null address incorrectly dereferences it: "
                + diagnostics(nullAddressOnly));
        auto checkedMemoryIntrinsics = psic::compile(
            "func void copy i8* destination i8* source u64 size { "
            "#memcpy destination source size; #memset destination 0 size; ret; }");
        check(checkedMemoryIntrinsics.success
                && checkedMemoryIntrinsics.ir.find("memory.nonzero") != std::string::npos
                && checkedMemoryIntrinsics.ir.find("@llvm.trap") != std::string::npos,
            "memory intrinsics do not guard null pointers for nonzero ranges: "
                + diagnostics(checkedMemoryIntrinsics));
        auto escapedBytes = psic::compile(
            "func void copy_out i8* destination { i8 local = 1; "
            "i8* source = ref local; #memcpy destination source 1; ret; }");
        check(!escapedBytes.success
                && diagnostics(escapedBytes).find("copy bytes from local storage") != std::string::npos,
            "#memcpy allowed bytes from local storage to escape the function");
        auto localBytes = psic::compile(
            "func void copy_local { i8 source_value = 1; i8 destination_value = 0; "
            "i8* source = ref source_value; i8* destination = ref destination_value; "
            "#memcpy destination source 1; ret; }");
        check(localBytes.success,
            "#memcpy rejected a copy between local storage ranges: " + diagnostics(localBytes));
        auto typedNullLoad = psic::compile(
            "func i32 read_null { i32* pointer = null; i32 value = load pointer; ret value; }");
        check(typedNullLoad.success && typedNullLoad.ir.find("@llvm.trap") != std::string::npos,
            "typed null load did not lower to a language trap: " + diagnostics(typedNullLoad));
        auto pointerCopy = psic::compile("func i32* copy i32* p { i32* q = p; ret q; }");
        check(pointerCopy.success, "matching pointer assignment failed: " + diagnostics(pointerCopy));
        psic::CompileOptions x32Options;
        x32Options.targetTriple = "x86_64-unknown-linux-gnux32";
        auto x32PointerDifference = psic::compile(
            "func i32 distance i8* left i8* right { i32 count = sub left right; ret count; }",
            x32Options);
        check(x32PointerDifference.success
                && x32PointerDifference.ir.find("p:32:32") != std::string::npos
                && x32PointerDifference.ir.find("ptrtoint ptr") != std::string::npos
                && x32PointerDifference.ir.find("to i32") != std::string::npos,
            "x32 pointer difference did not use the ABI pointer width: "
                + diagnostics(x32PointerDifference));
        auto x32PointerOffset = psic::compile(
            "func i32* offset i32* pointer u64 index { "
            "i32* result = add pointer index; ret result; }", x32Options);
        check(x32PointerOffset.success
                && x32PointerOffset.ir.find("trunc i64") != std::string::npos
                && x32PointerOffset.ir.find("getelementptr i32, ptr") != std::string::npos,
            "x32 pointer arithmetic did not use the target pointer index width: "
                + diagnostics(x32PointerOffset));
        x32Options.output = psic::OutputKind::Object;
        auto x32Object = psic::compile(
            "func i32 distance i8* left i8* right { i32 count = sub left right; ret count; }",
            x32Options);
        check(x32Object.success && !x32Object.object.empty(),
            "x32 target did not emit an object: " + diagnostics(x32Object));
        x32Options.output = psic::OutputKind::LLVMIR;
        auto x32Syscall = psic::compile("entry main { #syscall 0; }", x32Options);
        check(!x32Syscall.success
                && diagnostics(x32Syscall).find("not available for the Linux x32 ABI")
                    != std::string::npos,
            "x32 target accepted a syscall using the wrong ABI");
        for (auto level : {psic::OptimizationLevel::O0, psic::OptimizationLevel::O1,
             psic::OptimizationLevel::O2, psic::OptimizationLevel::O3, psic::OptimizationLevel::Os, psic::OptimizationLevel::Oz}) {
            options.optimization = level;
            auto result = psic::compile("func i32 answer { i32 n = add 20 22; ret n; }", options);
            check(result.success, diagnostics(result));
            if (level != psic::OptimizationLevel::O0)
                check(result.ir.find("ret i32 42") != std::string::npos, "optimization did not fold arithmetic");
            options.output = psic::OutputKind::Object;
            auto object = psic::compile("func i32 answer { i32 n = add 20 22; ret n; }", options);
            check(object.success && !object.object.empty() && object.ir.empty(), diagnostics(object));
            options.output = psic::OutputKind::LLVMIR;
        }
        options.architecture = "wasm32";
        options.output = psic::OutputKind::Object;
        auto wasm = psic::compile("entry main { u32 n = #memory.grow 1; ret; }", options);
        check(wasm.success && wasm.object.size() > 8 && wasm.object[0] == 0 && wasm.object[1] == 'a', diagnostics(wasm));
        check(wasm.ir.empty(), "object compilation returned IR");
        options.architecture = "invalid";
        check(!psic::compile("", options).success, "invalid architecture accepted");
        options = {};
        options.optimization = static_cast<psic::OptimizationLevel>(99);
        check(!psic::compile("", options).success, "invalid optimization enum accepted");
        options = {};
        options.output = static_cast<psic::OutputKind>(99);
        check(!psic::compile("", options).success, "invalid output enum accepted");

        reject("/* unfinished", "unterminated block comment");
        reject(R"(entry main { i8* text = "unfinished\"; })", "unterminated string literal");
        reject("entry main { i32 x = 0x; }", "hexadecimal digits");
        reject("entry main { i32 x = 0o8; }", "octal digits");
        reject("const f64 too_large = " + std::string(400, '9') + ".0;",
            "floating-point literal is out of range");
        reject("entry main { i32 x = ;", "expected");
        reject("func void f { ret; } func void f { ret; }", "duplicate declaration");
        reject("func void f i32 a i32 a { ret; }", "duplicate argument");
        reject("struct Pair { i32 x i32 x }", "duplicate field");
        reject("struct i32 { i32 field }", "redefine builtin type");
        reject("entry main { i32 x; i64 x; }", "duplicate local declaration");
        reject("struct Node { Node next }", "recursive by-value");
        reject("entry main { i32:3 x; }", "power of two");
        reject("entry main { void x; }", "void is only valid");
        reject("entry main { i32[] values; }", "must be initialized from an array literal");
        reject("entry main { i32[] values = [1]; i32[]* pointer = ref values; }",
            "cannot have pointer levels outside the view");
        reject("func i32[] bad { ret; }", "cannot be returned");
        reject("func void external i32[] values;",
            "cannot be passed to external function declarations");
        reject("struct Bad { i32[] values }", "cannot be stored in structures");
        reject("i32[] values = [1, 2];", "cannot have global storage");
        reject("entry main { label x; label x; }", "duplicate label");
        reject("entry main { jmp; }", "requires 1 operand");
        reject("entry main { cjmp 1; }", "requires 2 operand");
        reject("entry main { label 12; }", "named operand");
        reject("entry main { jmp missing; }", "undefined label");
        reject("entry main { i32 x = add 1; }", "requires 2 operand");
        reject("entry main { i32 x = add 1 2 3; }", "requires 2 operand");
        reject("entry main { store 1 2; }", "requires a pointer");
        reject("entry main { i32 x = load 1; }", "pointer operand");
        reject("entry main { i32 x; x = load x; }", "explicit result type");
        reject("entry main { i32 x = not 1.0; }", "integer operand");
        reject("entry main { i32 x = and 1.0 2.0; }", "integer operands");
        reject("entry main { i32 x = add 1 2.0; }", "cannot mix integer and floating-point operands");
        reject("entry main { i32 x = missing; }", "undeclared register");
        reject("entry main { ret; i32 x = 1; }", "after a terminator");
        reject("func i32 f { i32 x = 1; }", "without a return");
        reject("i32* p = 12;", "numeric literal requires a scalar numeric destination");
        reject("i32 p = \"text\";", "string initializer");
        reject("func i8* bad i32 n { i8* p = n; ret p; }", "type mismatch");
        reject("func i32 bad i64* p { i32 n = load p; ret n; }", "pointee type");
        reject("func i32 bad { i32 n = load \"text\"; ret n; }", "pointee type");
        reject("i32* immutable_values = [1, 2]; "
               "func void bad { i32* pointer; i32** slot = ref pointer; "
               "store slot immutable_values; pointer[0] = 7; ret; }",
            "cannot modify immutable global array or string storage");
        reject("i32* immutable_values = [1, 2]; "
               "func void bad { i32* pointer; i32** slot = ref pointer; "
               "store slot immutable_values; i32* loaded = load slot; "
               "loaded[0] = 7; ret; }",
            "cannot modify immutable global array or string storage");
        reject("struct Box { i32* pointer } i32* immutable_values = [1, 2]; "
               "func void bad { Box value; value.pointer = immutable_values; "
               "value.pointer[0] = 7; ret; }",
            "cannot modify immutable global array or string storage");
        reject("struct Box { i32* pointer } i32* immutable_values = [1, 2]; "
               "func void bad { Box value; value.pointer = immutable_values; "
               "i32* loaded = value.pointer; loaded[0] = 7; ret; }",
            "cannot modify immutable global array or string storage");
        auto mutableAggregatePointer = psic::compile(
            "struct Box { i32* pointer } "
            "func i32 update i32* external { Box value; "
            "value.pointer = external; value.pointer[0] = 7; ret external[0]; }");
        check(mutableAggregatePointer.success,
            "immutable aggregate tracking rejected a mutable pointer field: "
                + diagnostics(mutableAggregatePointer));
        reject("struct Holder { i32* pointer } "
               "func Holder escape { i32 local = 1; i32* address = ref local; "
               "Holder value; value.pointer = address; ret value; }",
            "cannot return a pointer or address derived from local storage");
        reject("struct Holder { i32* pointer } "
               "func Holder escape { i32 local = 1; i32* address = ref local; "
               "Holder value; i32** slot = ref value.pointer; store slot address; "
               "ret value; }",
            "cannot return a pointer or address derived from local storage");
        auto returnedExternalPointerAggregate = psic::compile(
            "struct Holder { i32* pointer } "
            "func Holder preserve i32* external { Holder value; "
            "value.pointer = external; ret value; }");
        check(returnedExternalPointerAggregate.success,
            "local aggregate tracking rejected an external pointer: "
                + diagnostics(returnedExternalPointerAggregate));
        auto indirectlyStoredExternalPointerAggregate = psic::compile(
            "struct Holder { i32* pointer } "
            "func Holder preserve i32* external { Holder value; "
            "i32** slot = ref value.pointer; store slot external; ret value; }");
        check(indirectlyStoredExternalPointerAggregate.success,
            "local aggregate tracking rejected an indirect external pointer store: "
                + diagnostics(indirectlyStoredExternalPointerAggregate));
        reject("func i32* bad i64* p { i32* q = p; ret q; }", "pointer type mismatch");
        reject("func i32* bad i64* p { ret p; }", "pointer type mismatch");
        reject("func i32* take i32* p { ret p; } func i32 bad i64* q { i32* p = call take q; ret p; }", "pointer type mismatch");
        reject("func void bad i32* p i64 x { store p x; ret; }", "pointer type mismatch");
        reject("func void bad i32* p { store p 1; ret; }", "pointer type mismatch");
        reject("func i32* bad i32 n { i32* p = #ptrcast n; ret p; }", "requires a pointer operand");
        reject("func i32 bad i64 value { i32 extended = #sext value; ret 0; }",
            "requires a wider result");
        reject("func i32 bad { i32 value = add value 1; ret value; }",
            "use of undeclared register 'value'");
        reject("func i32 bad i32 value { i64 narrowed = #trunc value; ret 0; }",
            "requires a narrower result");
        reject("func u32 bad f32 value { u32 result = #fptosi value; ret result; }",
            "signed integer result");
        reject("func i32 bad f32 value { i32 result = #fptoui value; ret result; }",
            "unsigned integer result");
        reject("func f32 bad i8* p { f32 n = #ptrtoint p; ret n; }", "integer result type");
        reject("func i32* bad i32 n { i32* p = #inttoptr n; ret p; }", "operand width");
        reject("func u32 bad i8* p { u32 n = #ptrtoint p; ret n; }", "result width");
        for (const std::string& architecture : {"x86_64", "wasm32"}) {
            psic::CompileOptions pointerOptions;
            pointerOptions.architecture = architecture;
            auto mismatch = psic::compile(
                "func i32* bad i64* p { i32* q = p; ret q; }", pointerOptions);
            check(!mismatch.success && diagnostics(mismatch).find("pointer type mismatch") != std::string::npos,
                "pointer mismatch semantics changed for " + architecture + ": " + diagnostics(mismatch));
        }

        std::vector<std::future<bool>> calls;
        for (int i = 0; i < 12; ++i) calls.push_back(std::async(std::launch::async, [i] {
            psic::CompileOptions opts;
            opts.architecture = i % 2 ? "riscv64" : "wasm32";
            auto result = psic::compile("func i32 value { ret " + std::to_string(i) + "; }", opts);
            return result.success && result.ir.find("ret i32 " + std::to_string(i)) != std::string::npos;
        }));
        for (auto& call : calls) check(call.get(), "concurrent compilation failed");
        check(captured.str().empty(), "library printed diagnostics: " + captured.str());
        std::cout << "Library API, diagnostics, recovery, optimization, and concurrency checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
