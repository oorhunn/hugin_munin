#!/usr/bin/env python3
"""Run the instrumented C++ suite and generate an LLVM core/bridge coverage report."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/coverage')
    parser.add_argument('--report-only', action='store_true', help='Reuse the last run profiles')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    profiles = build / 'profiles'
    report = build / 'coverage-report'
    cache = build / 'CMakeCache.txt'
    if not cache.is_file() or '-fprofile-instr-generate -fcoverage-mapping' not in cache.read_text():
        raise SystemExit('Select an instrumented build configured with cmake --preset coverage')
    cov = subprocess.check_output(['xcrun', '--find', 'llvm-cov'], text=True).strip()
    profdata = subprocess.check_output(['xcrun', '--find', 'llvm-profdata'], text=True).strip()
    # The profile runtime and readers must come from the same selected Xcode/CLT toolchain.
    test_status = 0
    if not args.report_only:
        shutil.rmtree(profiles, ignore_errors=True)
        profiles.mkdir(parents=True)
        environment = dict(os.environ, LLVM_PROFILE_FILE=str(profiles / '%m-%p.profraw'))
        # Swift behavior is checked by normal CI; hm_tests only builds C++ executables.
        test_status = subprocess.run(
            ['ctest', '--test-dir', str(build), '--output-on-failure', '--parallel', '1',
             '-E', '^swift_behavior_test$'], env=environment, check=False).returncode
    raw = sorted(profiles.glob('*.profraw'))
    if not raw:
        raise SystemExit('No raw profiles: configure with --preset coverage and build hm_tests first')
    shutil.rmtree(report, ignore_errors=True)
    report.mkdir(parents=True)
    merged = report / 'coverage.profdata'
    run([profdata, 'merge', '-sparse', '--num-threads=2', *raw, '-o', merged])
    tests = json.loads(subprocess.check_output(
        ['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True))
    binaries = {Path(test['command'][0]) for test in tests['tests']
                if test.get('command') and Path(test['command'][0]).parent == build
                and not test['name'].startswith('swift_')}
    binaries.update(build / name for name in ('hm-service', 'hm-mcp', 'hm-run'))
    binaries = sorted(binaries)
    missing = [str(binary) for binary in binaries if not binary.is_file()]
    if missing:
        raise SystemExit('Missing coverage executables; build hm_tests: ' + ', '.join(missing))
    # Explicit source whitelist excludes tests, utilities, generated files, vendors and SDKs.
    sources = [str(ROOT / path) for path in subprocess.check_output(
        ['git', '-C', str(ROOT), 'ls-files', 'core'], text=True).splitlines()
               if Path(path).suffix in {'.cpp', '.mm', '.hpp', '.h'}]
    common = [str(binaries[0]), f'-instr-profile={merged}']
    common += [f'-object={binary}' for binary in binaries[1:]]
    common += ['--sources', *sources]
    run([cov, 'show', '-format=html', f'-output-dir={report / "html"}',
         '-show-branches=count', '-show-branch-summary', *common])
    with (report / 'summary.txt').open('w') as output:
        run([cov, 'report', '-show-branch-summary', *common], stdout=output)
    with (report / 'summary.json').open('w') as output:
        run([cov, 'export', '-summary-only', *common], stdout=output)
    print((report / 'summary.txt').read_text())
    print(f'HTML: {report / "html/index.html"}')
    if test_status:
        raise SystemExit(test_status)


if __name__ == '__main__':
    main()
