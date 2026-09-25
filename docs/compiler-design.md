# Compiler design

This note follows one source file through PSIC's internal representations. The
public API and a file map are in the root [README](../README.md#how-compilation-works).

## 1. Public entry point

`psic::compile` in `src/compiler.cpp` serializes calls and scopes diagnostics to
the result. It validates option enums, resolves architecture and OS through
`resolveTarget`, invokes the front end, then asks codegen for LLVM IR and either
optimizes that IR for return or emits an object in memory. It clears partial
output on failure. The CLI in `src/main.cpp` handles arguments and files around
this API; library callers do not need file I/O.

The CLI's `main` follows that boundary directly: parse options, read the named
file (or standard input when the name is `-`), build `CompileOptions`, call the
library, print returned diagnostics, and write the selected result. LLVM IR can
go to standard output; object bytes require a file. Compilation errors return
before output is opened, which preserves an existing output file on source errors.
`writeIR` and `writeObject` keep text and binary output handling separate; the
object writer checks for failures after closing the file.
`OptionSpec` in `src/main.cpp` is the table of accepted option spellings and
actions. Argument parsing resolves values first, then applies the action; when
both `-c` and `--emit-llvm` are present, the last output-mode option wins.
`--` ends option parsing, and `-` names standard input; LLVM IR may also use `-`
as its output destination, but object output requires a file.

Target selection lives in `src/target.cpp`. An explicit architecture name is
matched against the alias table; otherwise the architecture comes from the
normalized LLVM target triple, defaulting to x86-64. The OS is chosen from an
explicit OS name first, then inferred from the triple, defaulting to Linux (or
freestanding for WebAssembly without an OS/triple). When both an explicit value
and a triple are given, they must agree. The resolved enums guide codegen, while
the original triple is kept for LLVM module and object-target setup.

## 2. Front end

`Lexer::tokenize` in `src/lexer.cpp` scans source into `Token` values. Each token
contains its kind, spelling, and starting line and column. Comments and
whitespace are skipped. The parser takes ownership of the token vector and uses a
cursor into it; lookahead returns references to stored tokens, while consuming a
token returns a value that remains valid after the cursor advances. It builds a
`ProgramNode`:

- A declaration is an entry, struct, function, global, or constant.
- Function and entry bodies contain `CommandNode` values.
- Types and registers store their names plus pointer levels or accessors.
- Operand values are held in a program-owned arena. AST fields keep pointers to
  those values, so copies of declarations do not recursively copy nested arrays.

The parser handles syntax and literal decoding. `validateProgram` in
`src/validation.cpp` checks declaration uniqueness, operand counts, required
targets, names, pointer conversions, typed loads/stores, call arguments, and
by-value struct cycles. Its PSI `TypeEnvironment` is built from declarations
before function bodies are checked, so source-order does not change pointer
compatibility. It resolves register accessors from declared PSI types; it does
not query LLVM types. Backends consume the validated PSI contract and lower
explicit casts according to the selected target's pointer representation.
Special-instruction-specific machine constraints remain in codegen.

Pointer compatibility is exact in both base type and pointer depth, with `null`
as the sole implicit pointer conversion. `ref` adds one pointer level to the
selected lvalue type, including when that lvalue already holds a pointer.
Indexing consumes one level; field selection requires a struct value. A typed
`load` and `store` must agree with the pointer's pointee type. Reinterpretation
uses explicit `#ptrcast`; integer-address conversion uses `#ptrtoint` and
`#inttoptr` and requires an integer width matching an integral target pointer
representation. Those width and integral-pointer checks are target properties,
not assumptions about LLVM's pointer type representation.

The unprefixed primary instructions are a target-independent abstract machine.
Integer `add`/`sub`/`mul` operate modulo the declared bit width; division and
remainder trap for zero divisors and signed minimum divided by `-1`. Shift
counts are unsigned bit patterns and trap when at least the operand width;
`rsh` is arithmetic for signed types and logical for unsigned types. Binary
numeric operations convert the right operand to the left operand's type before
execution, with integer widening selected from the left type's signedness.
Floating comparisons use ordered predicates, so all comparisons with NaN,
including `neq`, are false. Backends must implement these rules rather than
inherit the host ISA's edge behavior. Special `#` operations are extensions
whose availability is explicitly constrained by their target contract.

Struct-cycle validation runs a depth-first walk over fields without pointer
levels: those fields contribute to a struct's finite LLVM size, while pointer
fields do not. The walk tracks structs currently being visited to detect a
cycle, and structs already completed so shared subgraphs are only checked once.

At command start, `parseCommand` first recognizes instruction keywords, then
distinguishes a declaration (`type name`), an assignment (`name =`), and an
accessor assignment (`name[index] =` or `name.field =`). A target is followed by
`=` unless it is a declaration without an initializer. The parser then reads an
instruction and its operands, a direct value, or just the terminating semicolon.
Braces delimit declaration and function bodies. String escapes are decoded by
the parser after the lexer has collected the quoted token. Parsed `ValueNode`s
are allocated in `Parser::ownedValues` and transferred to `ProgramNode` when
parsing completes; commands and declarations retain pointers into that arena.

## 3. LLVM lowering

`compileProgram` in `src/codegen_module.cpp` creates a fresh `State`, LLVM context,
and module for each compilation. `State` in `src/codegen_state.hpp` is the symbol
table shared by the lowering functions. It contains type mappings, function and
global declarations, struct field metadata, and the local variables and labels
for the function currently being generated.

Before lowering declarations, `declareBuiltinTypes` builds the builtin type map
from PSI names to LLVM scalar and fixed-vector types. Signed and unsigned integer
spellings share an LLVM integer type; signedness is kept in the PSI `TypeNode`
and consulted when choosing operations. `declareStructTypes` adds struct shells
and field layouts afterward.

The declaration walks are ordered because later declarations can refer to earlier
or later types and functions:

1. Create LLVM struct types as opaque named shells. This allows a struct field to
   refer to another declared struct before its body is filled in.
2. Fill each struct shell with its resolved field types and record field indexes.
3. `declareFunctionsAndGlobals` creates function and global/constant symbols so
   function bodies can refer to declarations regardless of source order.
4. `generateProgramBodies` lowers entry and function bodies, then the module is
   verified before its IR is returned.

Within a function, `generateFunctionBody` creates the entry block, puts arguments
in local storage, and predeclares labels. `processCommand` lowers each command.
Locals use LLVM stack slots; register accessors resolve to addresses through
`resolveRegisterAddress`. `computeCommandValue` lowers expressions and
instructions, while special registers and architecture-specific instructions are
handled by `src/codegen_registers.cpp` and the special-instruction paths in
`src/codegen_instructions.cpp`.

Commands without a result target take the separate
`processUntargetedInstruction` path. It handles returns, stores, jumps, labels,
and calls whose result is discarded. Register-targeted commands instead compute
a value and store it in a local or resolved register address. New local slots are
handled by `processLocalDeclaration`; writes to existing registers go through
`processRegisterAssignment`.

`processValue` turns runtime AST values into LLVM values. Global and constant
initializers use `buildConstant`, which only accepts values LLVM can represent as
constants. Both dispatch by `ValueKind` and read the AST without copying or
mutating it.

Numeric literals have a fixed initial LLVM type: integer literals are `i32` and
decimal literals are `f32`. When a binary instruction is lowered, its right
operand is coerced to the left operand's LLVM type. If the left operand is a
typed register, its PSI declaration supplies signedness; untyped values such as
literals default to signed behavior. That signedness selects integer division,
remainder, comparisons, right shifts, and integer widening. Floating comparisons
use ordered predicates, so NaN comparisons do not report equal or ordered
relations. `land` and `lor` lower to integer truth tests combined with LLVM
boolean operations; they are not short-circuit control flow.

`computeCommandValue` handles ordinary instructions after parsing has built the
command and validation has checked the applicable operand counts. It handles
calls, references, loads, and unary operations directly. It delegates scalar
binary operators to `processBinaryInstruction`, which evaluates and coerces both
operands, checks their numeric types, and selects the LLVM operation. Special
instructions use the separate `processSpecialInstruction` dispatcher described
above. A declared local's type is passed into lowering for operations such as
`load` and `vsplat`, which need a result type before storing.

`buildSpecialRegisterTable` in `src/codegen_registers.cpp` maps supported names
for the selected architecture to register widths and LLVM inline-assembly
constraints. Aliases such as `sp` and `fp` share register metadata; system
register entries also carry read and write assembly templates. Private helpers
build each architecture's register family, then add that target's system
registers. Reads and writes are checked against the resulting table, with a few
target-specific intrinsics handled directly.

Special instructions enter through `processSpecialInstruction`. It normalizes
dotted names to underscore names, handles WebAssembly memory operations, then
delegates target-specific fixed assembly to `processNativeInstruction`. The
remaining dispatcher sends scalar intrinsics, vector operations, and atomics to
focused helpers. Architecture checks stay at the operation that needs them;
portable arithmetic uses LLVM operations or intrinsics instead of embedding
target assembly.

## 4. Output

The module is serialized as LLVM IR first. `src/codegen.cpp` reparses the text in
a fresh LLVM context, installs the diagnostic handler, and applies the selected
optimization pipeline. `buildTargetMachine` initializes LLVM target components
once before any target lookup; object output then uses the machine's emission
pass manager to write into an in-memory buffer. The public API returns either
the requested IR string or object bytes, along with diagnostics.

## 5. Diagnostics and ownership

Compiler diagnostics go through `src/logging.cpp`. During a library call,
`DiagnosticScope` redirects them into `CompileResult::diagnostics` and restores
the previous sink when the call exits. Ordinary source and target errors are
diagnostics; allocation failures are allowed to propagate as exceptions.

The logger keeps a thread-local error count for control flow and sends each
error, warning, or note through one emitter. With a diagnostic sink installed it
appends a structured severity/message pair; otherwise it prints a prefixed line
to standard error. Buffered `ErrorStream` messages are converted to ordinary
error log calls when the stream is destroyed.

Each compile owns its tokens, AST, LLVM context, module, and codegen state. AST
value pointers remain valid because the `ProgramNode` owns the arena for the full
lowering call. The logger's sink and error count are thread-local and restored
around each API call. Target components are registered once with `std::call_once`;
public API calls are currently serialized around the compiler core.
