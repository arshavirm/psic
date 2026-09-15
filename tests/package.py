"""Build and execute an external client against a relocated installation."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
for name in ['cmake', 'ctest', 'build', 'source', 'config', 'llvm', 'generator', 'platform', 'compiler']:
    parser.add_argument('--' + name, required=True)
args = parser.parse_args()


def run(command, env=None):
    result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=120)
    if result.returncode:
        raise RuntimeError(f'{command}\n{result.stdout}\n{result.stderr}')


with tempfile.TemporaryDirectory(prefix='psic package ') as directory:
    root = Path(directory)
    stage = root / 'stage'
    run([args.cmake, '--install', args.build, '--prefix', str(stage), '--config', args.config])
    moved = root / 'relocated install'
    shutil.move(stage, moved)
    configs = list(moved.rglob('PSICConfig.cmake'))
    if len(configs) != 1:
        raise RuntimeError('installed package config missing or ambiguous')
    client = root / 'client build'
    command = [args.cmake, '-S', args.source, '-B', str(client), '-G', args.generator,
               f'-DPSIC_DIR={configs[0].parent}', f'-DLLVM_DIR={args.llvm}',
               f'-DCMAKE_CXX_COMPILER={args.compiler}', f'-DCMAKE_BUILD_TYPE={args.config}']
    if args.platform:
        command += ['-A', args.platform]
    run(command)
    run([args.cmake, '--build', str(client), '--config', args.config, '--parallel', '2'])
    env = os.environ.copy()
    env['PATH'] = str(moved / 'bin') + os.pathsep + env.get('PATH', '')
    run([args.ctest, '--test-dir', str(client), '-C', args.config, '--output-on-failure'], env)
    embedded = root / 'embedded build'
    embed_command = [args.cmake, '-S', args.source, '-B', str(embedded), '-G', args.generator,
                     f'-DPSIC_SOURCE_DIR={Path(args.source).resolve().parents[1]}',
                     f'-DLLVM_DIR={args.llvm}', f'-DCMAKE_CXX_COMPILER={args.compiler}',
                     f'-DCMAKE_BUILD_TYPE={args.config}']
    if args.platform:
        embed_command += ['-A', args.platform]
    run(embed_command)
    run([args.cmake, '--build', str(embedded), '--config', args.config, '--parallel', '2'])
    run([args.ctest, '--test-dir', str(embedded), '-C', args.config, '--output-on-failure'])
print('Relocated installed and add_subdirectory consumers built and ran.')
