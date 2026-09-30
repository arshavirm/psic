"""CLI contract checks, including output preservation and diagnostics."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
count = 0


def run(args, source=None, expected=0):
    global count
    count += 1
    result = subprocess.run([compiler, *args], input=source, text=True, capture_output=True, timeout=30)
    if result.returncode != expected:
        raise AssertionError(f'{args}: {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result


def contains(text, expected):
    if expected not in text:
        raise AssertionError(f'expected {expected!r} in {text!r}')


contains(run(['--help']).stdout, 'wasm32')
contains(run(['--help']).stdout, '-O0')
contains(run(['--help']).stdout, '-O3')
contains(run(['--version']).stdout, 'psic 0.1.0')
run(['--jit', '--entry', 'finish', '-'], 'func i32 finish { ret 7; }', expected=7)
contains(run(['-c', '--emit-llvm', 'missing.psi'], expected=2).stderr, 'choose one')
contains(run(['--jit', '-o', 'ignored.o', '-'], '', expected=2).stderr,
    'cannot be combined with output options')
contains(run(['--entry', 'finish', '-'], '', expected=2).stderr, 'requires --jit')
contains(run(['--jit', '--entry', 'first', '--entry', 'second', '-'], '', expected=2).stderr,
    "conflicting values supplied for '--entry'")
contains(run(['--target', 'x86_64-unknown-linux-gnu', '--target', 'aarch64-unknown-linux-gnu', '-'], '', expected=2).stderr,
    "conflicting values supplied for '--target'")
contains(run(['-o', 'first.o', '--output', 'second.o', '-'], '', expected=2).stderr,
    "conflicting values supplied for '-o/--output'")
run(['--arch', 'x86_64', '--arch', 'x86_64', '--emit-llvm', '-o', '-', '-'], '')
contains(run([], expected=2).stderr, 'no input files')
contains(run(['--bad-option'], expected=2).stderr, 'unknown option')
for option in ['-o', '--arch', '--os', '--target']:
    contains(run([option], expected=2).stderr, 'requires an argument')
with tempfile.TemporaryDirectory(prefix='psic cli ') as folder:
    root = Path(folder)
    source = root / 'with spaces.psi'
    source.write_text('func i32 answer { i32 x = add 20 22; ret x; }')
    output = root / 'answer.ll'
    run([str(source), '--emit-llvm', '-O2', '-o', str(output)])
    contains(output.read_text(), 'ret i32 42')
    result = run(['--emit-llvm', '-O0', '-', '-o', '-'], source.read_text())
    contains(result.stdout, 'define i32 @answer')
    if result.stderr:
        raise AssertionError(f'unexpected diagnostics: {result.stderr}')
    result = run(['--emit-llvm', '-v', '-', '-o', '-'], source.read_text())
    contains(result.stderr, 'Compiling')
    if 'psic:' in result.stdout:
        raise AssertionError('status output contaminated LLVM IR stdout')
    run([str(source), '--emit-llvm'])
    if not source.with_suffix('.ll').exists():
        raise AssertionError('default output path missing')
    run([str(source), str(source)], expected=2)
    run([str(root / 'missing.psi')], expected=1)
    sentinel = 'preserve existing output'
    output.write_text(sentinel)
    run(['--emit-llvm', '-', '-o', str(output)], 'entry main { i32 x = add; }', expected=1)
    if output.read_text() != sentinel:
        raise AssertionError('failed compilation overwrote an existing output')
    run(['--emit-llvm', '-', '-o', str(root / 'absent' / 'file.ll')], '', expected=1)
    run(['-', '-o', '-'], '', expected=1)
    parse_error = run(['--emit-llvm', '-', '-o', '-'],
        'entry main {\n i32 x = "oops;\n}', expected=1)
    contains(parse_error.stderr, '<stdin>:2:')
    contains(parse_error.stderr, 'i32 x = "oops;')
    located = run(['--emit-llvm', '-', '-o', '-'],
        'entry main {\n  i32 value = add 1;\n}', expected=1)
    contains(located.stderr, '<stdin>:2:3:')
    contains(located.stderr, '  i32 value = add 1;')
    contains(located.stderr, '  ^')
    tabbed = run(['--emit-llvm', '-', '-o', '-'],
        'entry main {\n\t i32 value = add 1;\n}', expected=1)
    contains(tabbed.stderr, '<stdin>:2:3:')
    if '         ^' not in tabbed.stderr:
        raise AssertionError(f'caret did not account for tab expansion: {tabbed.stderr!r}')
    backend_located = run(['--emit-llvm', '-', '-o', '-'],
        'entry main {\n  #unknown_operation;\n}', expected=1)
    contains(backend_located.stderr, '<stdin>:2:3:')
    contains(backend_located.stderr, '#unknown_operation;')
print(f'Passed {count} CLI contract checks.')
