#!/usr/bin/env python3
"""Cross-target integration tests. Requires an LLVM build with all tested backends."""
import pathlib
import subprocess
import sys
import tempfile

COMPILER = str(pathlib.Path(sys.argv[1]).resolve())
COUNT = 0

# Expected ELF e_machine and byte order; WASM has its own magic/header.
TARGETS = {
    'x86': (3, 'little'), 'x86_64': (62, 'little'),
    'arm': (40, 'little'), 'aarch64': (183, 'little'),
    'wasm32': None, 'wasm64': None,
    'riscv32': (243, 'little'), 'riscv64': (243, 'little'),
    'ppc': (20, 'big'), 'ppc64': (21, 'big'), 'ppc64le': (21, 'little'),
    'mips': (8, 'big'), 'mipsel': (8, 'little'),
    'mips64': (8, 'big'), 'mips64el': (8, 'little'),
    'loongarch64': (258, 'little'), 's390x': (22, 'big'),
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def compile_source(arch, body, *, options=(), error=None, ir=False, raw=False, infer=False):
    global COUNT
    COUNT += 1
    source = body if raw else 'entry main { ' + body + ' ret; }'
    with tempfile.TemporaryDirectory(prefix='psic-test-') as folder:
        output = pathlib.Path(folder) / ('test.ll' if ir else 'test.o')
        args = [COMPILER, *([] if infer else ['-march', arch]), *options, '-', '-o', str(output)]
        if ir:
            args.append('--emit-llvm')
        result = subprocess.run(args, input=source, text=True, capture_output=True, timeout=30)
        context = f'{arch} {options}: {source}\n{result.stderr}'
        if error:
            require(result.returncode > 0, context)
            require(error in result.stderr, context)
            require(not output.exists(), context)
            return
        require(result.returncode == 0, context)
        data = output.read_bytes()
        if ir:
            text = data.decode()
            require('target datalayout = ' in text, context)
            return text
        if TARGETS[arch] is None:
            require(data[:8] == b'\x00asm\x01\x00\x00\x00', context)
        else:
            machine, endian = TARGETS[arch]
            require(data[:4] == b'\x7fELF', context)
            is32 = arch in ['x86', 'arm', 'riscv32', 'ppc', 'mips', 'mipsel']
            require(data[4] == (1 if is32 else 2), context)
            require(data[5] == (1 if endian == 'little' else 2), context)
            require(int.from_bytes(data[18:20], endian) == machine, context)
        return data


for arch in TARGETS:
    for opt in ['-O0', '-O2']:
        compile_source(arch, '#nop; u32 n = #clz 1; u32 p = #popcnt n;', options=[opt])
        compile_source(arch, 'f32 a = 1.0; f32 b = 2.0; f32 c = #fadd a b;', options=[opt])

INSTRUCTIONS = {
    'x86': ['pause', 'lfence', 'sfence', 'mfence'],
    'x86_64': ['pause', 'lfence', 'sfence', 'mfence'],
    'arm': ['yield', 'dmb', 'dsb', 'isb', 'wfi', 'wfe', 'sev'],
    'aarch64': ['yield', 'dmb', 'dsb', 'isb', 'wfi', 'wfe', 'sev', 'sevl'],
    'riscv32': ['ecall', 'ebreak', 'wfi', 'fence_i', 'fence.i'],
    'riscv64': ['ecall', 'ebreak', 'wfi', 'fence_i', 'fence.i'],
    'ppc': ['sync', 'lwsync', 'isync', 'eieio'],
    'ppc64': ['sync', 'lwsync', 'isync', 'eieio'],
    'ppc64le': ['sync', 'lwsync', 'isync', 'eieio'],
    'mips': ['sync'], 'mipsel': ['sync'], 'mips64': ['sync'], 'mips64el': ['sync'],
    'loongarch64': ['dbar', 'ibar'], 's390x': ['serialize'],
}
for arch, instructions in INSTRUCTIONS.items():
    for instruction in instructions:
        compile_source(arch, f'#{instruction};')
        compile_source(arch, f'#{instruction} 1;', error='takes no operands')

REGISTERS = {
    'x86': ['eax', 'xmm0', 'mxcsr'],
    'x86_64': ['rax', 'r15d', 'xmm15', 'mxcsr'],
    'arm': ['r0', 'r13', 'r14', 'apsr'],
    'aarch64': ['x0', 'w30', 'nzcv', 'fpcr', 'fpsr', 'tpidr_el0'],
    'riscv32': ['x10', 'a0', 's11', 'fp'], 'riscv64': ['x10', 'a0', 's11', 'fp'],
    'ppc': ['r3', 'lr', 'ctr', 'xer'], 'ppc64': ['r3', 'lr', 'ctr', 'xer'],
    'ppc64le': ['r3', 'lr', 'ctr', 'xer'],
    'mips': ['r2', 'v0', 'hi', 'lo'], 'mipsel': ['r2', 'v0', 'hi', 'lo'],
    'mips64': ['r2', 'a4', 'a7', 't0', 'hi', 'lo'],
    'mips64el': ['r2', 'a4', 'a7', 't0', 'hi', 'lo'],
    'loongarch64': ['r4', 'a0', 'a7'], 's390x': ['r2', 'f0'],
}
for arch, registers in REGISTERS.items():
    for register in registers:
        ty = 'f32' if register.startswith('xmm') else 'f64' if register == 'f0' else 'u64'
        compile_source(arch, f'{ty} value = %{register}; %{register} = value;')

READ_ONLY = {
    'aarch64': ['cntvct_el0', 'cntfrq_el0'],
    'riscv32': ['cycle', 'time', 'instret', 'cycleh', 'timeh', 'instreth', 'zero', 'x0'],
    'riscv64': ['cycle', 'time', 'instret', 'zero', 'x0'],
    'mips': ['zero', 'r0'], 'mips64': ['zero', 'r0'],
    'loongarch64': ['zero', 'r0'], 'wasm32': ['memory_pages'], 'wasm64': ['memory_pages'],
}
for arch, registers in READ_ONLY.items():
    for register in registers:
        compile_source(arch, f'u64 value = %{register};')
        compile_source(arch, f'%{register} = 1;', error='read-only')

for arch, bits in [('wasm32', 32), ('wasm64', 64)]:
    for opt in ['-O0', '-O2']:
        compile_source(arch, f'u{bits} size = #memory.size; u{bits} old = #memory.grow 1;', options=[opt])
        compile_source(arch, 'u64 size = #memory_size; u64 old = #memory_grow 0;', options=[opt, '-mos', 'wasi'])
    ir = compile_source(arch, f'u{bits} size = #memory_size; u{bits} old = #memory_grow size;', ir=True)
    require(f'@llvm.wasm.memory.size.i{bits}' in ir, "architecture expectation failed: f'@llvm.wasm.memory.size.i{bits}' in ir")
    require(f'@llvm.wasm.memory.grow.i{bits}' in ir, "architecture expectation failed: f'@llvm.wasm.memory.grow.i{bits}' in ir")
    require('asm sideeffect' not in ir, "architecture expectation failed: 'asm sideeffect' not in ir")
    compile_source(arch, '#memory_size 0;', error='operand count')
    compile_source(arch, '#memory_grow;', error='operand count')
    compile_source(arch, '#memory_grow 1.0;', error='integer page count')
    compile_source(arch, '%rax = 0;', error="isn't a recognized register")
    compile_source(arch, '#syscall 1;', error='only the Linux')
    compile_source(arch, '', options=['-mos', 'linux'], error='requires OS none or wasi')

for arch in ['x86', 'x86_64', 'arm', 'aarch64', 'riscv32', 'riscv64']:
    compile_source(arch, 'u64 result = #syscall 1 2 3 4 5 6 7;')
    compile_source(arch, '#syscall 1;', options=['-mos', 'none'], error='only the Linux')

compile_source('x86_64', 'u64 tick = #rdtsc;')
compile_source('x86_64', '#rdtsc 1;', error='takes no operands')
compile_source('x86_64', '#memory_size;', error='requires WebAssembly')
compile_source('x86_64', '#syscall 1;', options=['-mos', 'windows'], error='only the Linux')
compile_source('aarch64', '#pause;', error='only available on x86')
compile_source('riscv64', '#yield;', error='only available on ARM')
compile_source('wasm32', '#dmb;', error='unsupported special instruction')
compile_source('x86_64', '', options=['-target', 'wasm32-unknown-unknown'], error='architecture conflicts')
compile_source('wasm32', '', options=['-target', 'wasm32-unknown-wasi', '-mos', 'none'], error='OS conflicts')
compile_source('riscv64', '', options=['-mos', 'darwin'], error='supports Linux or freestanding')
compile_source('x86_64', '', options=['-mos', 'wasi'], error='WASI requires WebAssembly')
for arch, triple in [('wasm32', 'wasm32-unknown-wasi'), ('riscv64', 'riscv64-unknown-none-elf'),
                     ('x86_64', 'x86_64-pc-linux-gnu')]:
    ir = compile_source(arch, '', options=['-target', triple], ir=True)
    require(f'target triple = "{triple}"' in ir, 'architecture expectation failed: f\'target triple = "{triple}"\' in ir')

# Exercise inference without --arch, rather than merely checking conflicts.
import re
for arch in TARGETS:
    ir = compile_source(arch, '', ir=True)
    triple = re.search(r'target triple = "([^"]+)"', ir).group(1)
    compile_source(arch, '#nop;', options=['--target', triple], infer=True)
    compile_source(arch, '#nop;', options=['--os', 'none'])
compile_source('arm', '#dmb;', options=['--target', 'armv7a-none-eabihf'], infer=True)
compile_source('x86_64', '', options=['--target', 'x86_64-unknown-typo'], error='unsupported OS')
print(f'Passed {COUNT} architecture integration cases.')
