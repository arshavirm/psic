#include <psic/compiler.hpp>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <fstream>

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
        auto second = psic::compile("func i32 sum { ret 7; }");
        check(second.success && second.ir.find("ret i32 7") != std::string::npos, "state leaked between compilations");
        for (auto level : {psic::OptimizationLevel::O0, psic::OptimizationLevel::O1,
             psic::OptimizationLevel::O2, psic::OptimizationLevel::O3, psic::OptimizationLevel::Os, psic::OptimizationLevel::Oz}) {
            options.optimization = level;
            auto result = psic::compile("func i32 answer { i32 n = add 20 22; ret n; }", options);
            check(result.success, diagnostics(result));
            if (level != psic::OptimizationLevel::O0)
                check(result.ir.find("ret i32 42") != std::string::npos, "optimization did not fold arithmetic");
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
        reject("entry main { i32 x = 0x; }", "hexadecimal digits");
        reject("entry main { i32 x = ;", "expected");
        reject("func void f { ret; } func void f { ret; }", "duplicate declaration");
        reject("func void f i32 a i32 a { ret; }", "duplicate argument");
        reject("struct Pair { i32 x i32 x }", "duplicate field");
        reject("struct i32 { i32 field }", "redefine builtin type");
        reject("entry main { i32 x; i64 x; }", "duplicate local declaration");
        reject("struct Node { Node next }", "recursive by-value");
        reject("entry main { i32:3 x; }", "power of two");
        reject("entry main { void x; }", "void is only valid");
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
        reject("entry main { i32 x = missing; }", "undeclared register");
        reject("entry main { ret; i32 x = 1; }", "after a terminator");
        reject("func i32 f { i32 x = 1; }", "without a return");
        reject("i32* p = 12;", "numeric global initializer");
        reject("i32 p = \"text\";", "string initializer");

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
