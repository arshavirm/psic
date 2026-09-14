#include <psic/compiler.hpp>
#include <iostream>
int main()
{
    auto result = psic::compile("func i32 answer { ret 42; }");
    if (!result.success || result.ir.find("ret i32 42") == std::string::npos) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.message << '\n';
        return 1;
    }
    return 0;
}
