#include <psic/compiler.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    std::ifstream input(argv[1]);
    if (!input) return 2;
    std::ostringstream text;
    text << input.rdbuf();
    psic::CompileOptions options;
    options.moduleName = "runtime-tests";
    options.targetTriple = argv[3];
    options.output = psic::OutputKind::Object;
    options.optimization = std::string(argv[4]) == "O0" ? psic::OptimizationLevel::O0 : psic::OptimizationLevel::O2;
    auto result = psic::compile(text.str(), options);
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.message << '\n';
    if (!result.success) return 1;
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(result.object.data()), result.object.size());
    return output ? 0 : 1;
}
