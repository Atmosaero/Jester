"""Run native Atom smoke and editor GUI tests with verified backend selection.

Python 3.9+, standard library only. Uses an already built Windows SDK project.
Reports and actual framebuffer captures stay under ignored artifacts/.
"""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import time


def read_text(path):
    data = path.read_bytes()
    encoding = 'utf-16' if data[:2] in (b'\xff\xfe', b'\xfe\xff') else 'utf-8-sig'
    return data.decode(encoding, errors='replace')


def execute(command, project, folder, backend, scale, timeout, language='en'):
    folder.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['QT_SCALE_FACTOR'] = str(scale)
    env['JESTER_PREFERENCES_FILE'] = str(folder/'preferences.json')
    (folder/'preferences.json').write_text(json.dumps({'format':'JesterPreferences','version':1,'language':language}),encoding='utf-8')
    env['JESTER_VISUAL_TEST_OUTPUT'] = str(folder)
    env['JESTER_RHI'] = backend
    env.pop('QT_QPA_PLATFORM',None)
    started = time.monotonic()
    timed_out = False
    with (folder / 'engine.log').open('wb') as log:
        process = subprocess.Popen(command, cwd=project, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()  # Only the process created by this test.
            code = process.wait(timeout=15)
    log_text = read_text(folder / 'engine.log')
    actual = [r.lower() for r in re.findall(r'Registering\s+(dx12|vulkan)\s+RHI', log_text, re.I)]
    errors = len(re.findall(r'Trace::(?:Error|Assert)', log_text))
    return dict(backend=backend, scale=scale, language=language, exitCode=code, timedOut=timed_out,
                executable=str(command[0]),executableSha256=hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
                seconds=round(time.monotonic()-started, 2), registeredRhi=actual,
                traceErrors=errors, engineLog=str(folder / 'engine.log'),
                passed=code == 0 and not timed_out and errors == 0 and actual == [backend])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root', type=Path, default=root)
    parser.add_argument('--bin-dir', type=Path, help='Default: <project-root>/build/bin/profile')
    parser.add_argument('--output-dir', type=Path, default=root / 'artifacts/rhi-validation')
    parser.add_argument('--backends', nargs='+', choices=['dx12', 'vulkan'], default=['dx12', 'vulkan'])
    parser.add_argument('--scales', nargs='+', type=float, choices=[1, 1.5], default=[1])
    parser.add_argument('--smoke-scales', nargs='+', type=float, choices=[1, 1.5], default=[1, 1.5])
    parser.add_argument('--suites', nargs='+', choices=['Ui', 'Mvp', 'Visual', 'Automation', 'Notes', 'PhysicsUi', 'Motion', 'UxScope'],
                        default=['Ui', 'Mvp', 'Visual', 'Automation', 'Notes', 'PhysicsUi', 'Motion', 'UxScope'])
    parser.add_argument('--skip-smoke', action='store_true')
    parser.add_argument('--smoke-only', action='store_true')
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--language',choices=['en','ru','system'],default='en')
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error('--timeout must be positive')
    if args.skip_smoke and args.smoke_only:
        parser.error('--skip-smoke and --smoke-only cannot be used together')
    project = args.project_root.resolve()
    binaries = (args.bin_dir or project / 'build/bin/profile').resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = binaries / 'Jester.exe'
    identity = subprocess.run([str(executable), '--build-info'], cwd=project,
                              capture_output=True, text=True, encoding='utf-8',
                              errors='replace', timeout=15, check=True).stdout.strip()
    environment = dict(buildInfo=identity, executable=str(executable),
                       executableSha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                       projectRoot=str(project), platform=platform.platform())
    (output / 'environment.json').write_text(json.dumps(environment, indent=2) + '\n', encoding='utf-8')
    results = []

    def save(result, folder):
        results.append(result)
        (folder / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        summary = dict(environment=environment, passed=all(r['passed'] for r in results), results=results)
        (output / 'results.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
        print(f"{result['backend']} {result['scale']:g} {result['suite']}: "
              f"{'PASS' if result['passed'] else 'FAIL'}", flush=True)

    for backend in args.backends:
        if not args.skip_smoke:
            # Smoke output names are fixed in the application. Preserve existing
            # evidence and restore it even if a new native run fails.
            fixed = ['poc-smoke.json', 'poc-viewport.png', 'poc-frame.ppm']
            backup = output / backend / 'previous-poc'
            backup.mkdir(parents=True, exist_ok=True)
            existed = []
            for name in fixed:
                path = project / 'artifacts' / name
                if path.exists():
                    shutil.copy2(path, backup / name)
                    existed.append(name)
            try:
                for scale in args.smoke_scales:
                    folder = output / backend / f'smoke-{scale:g}'
                    for name in fixed:
                        (project / 'artifacts' / name).unlink(missing_ok=True)
                    command = [str(binaries / 'Jester.exe'), f'--project-path={project}',
                               f'--rhi={backend}', '--unattended', '--smoke']
                    result = execute(command, project, folder, backend, scale, min(args.timeout, 90),args.language)
                    result['suite'] = 'Smoke'
                    report = project / 'artifacts/poc-smoke.json'
                    result['report'] = json.loads(read_text(report)) if report.exists() else None
                    result['passed'] &= bool(result['report'] and result['report'].get('passed'))
                    for name in fixed:
                        path = project / 'artifacts' / name
                        if path.exists():
                            shutil.copy2(path, folder / name)
                    save(result, folder)
            finally:
                for name in fixed:
                    path = project / 'artifacts' / name
                    if name in existed:
                        shutil.copy2(backup / name, path)
                    else:
                        path.unlink(missing_ok=True)
        if args.smoke_only:
            continue
        for scale in args.scales:
            for suite in args.suites:
                folder = output / backend / f'{suite.lower()}-{scale:g}'
                folder.mkdir(parents=True, exist_ok=True)
                test_log = folder / 'tests.txt'
                test_log.unlink(missing_ok=True)
                command = [str(binaries / f'Jester{suite}Tests.exe'), f'--project-path={project}',
                           f'--rhi={backend}', '--unattended', '-o', f'{test_log},txt']
                result = execute(command, project, folder, backend, scale, args.timeout,args.language)
                result['suite'] = suite
                totals = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped',
                                   read_text(test_log) if test_log.exists() else '')
                result['totals'] = dict(zip(['passed', 'failed', 'skipped'], map(int, totals.groups()))) if totals else None
                result['passed'] &= bool(totals and int(totals[2]) == 0)
                save(result, folder)
    return 0 if results and all(r['passed'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
