"""Measure CPU cost, preparation callback pacing and separate framebuffer readback.

Uses the deterministic render fixtures and an existing JesterPerformanceTests
build. Callback intervals are not GPU timings or physical presentation times.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root',type=Path,default=root)
    parser.add_argument('--output-dir',type=Path,default=root/'artifacts/render-performance')
    parser.add_argument('--backends',nargs='+',choices=['dx12','vulkan'],default=['dx12','vulkan'])
    parser.add_argument('--scales',nargs='+',type=float,choices=[1,1.5],default=[1])
    parser.add_argument('--cpu',action='store_true',help='Request the CPU fallback for parity comparisons')
    args=parser.parse_args()
    project=args.project_root.resolve();output=args.output_dir.resolve();output.mkdir(parents=True,exist_ok=True)
    binary=project/'build/bin/profile'
    identity=subprocess.check_output([str(binary/'Jester.exe'),'--build-info'],text=True).strip()
    results=dict(buildInfo=identity,executableSha256=hashlib.sha256((binary/'Jester.exe').read_bytes()).hexdigest(),
        fixtureVersion='jester-render-fixtures/1',cpuRequested=args.cpu,runs=[])
    for backend in args.backends:
        for scale in args.scales:
            folder=output/backend/f'{scale:g}';folder.mkdir(parents=True,exist_ok=True)
            env={**os.environ,'QT_SCALE_FACTOR':str(scale),'JESTER_BENCHMARK_OUTPUT':str(folder),
                 'VK_LOADER_LAYERS_DISABLE':'~implicit~'}
            env.pop('QT_QPA_PLATFORM',None)
            if args.cpu:env['JESTER_BENCHMARK_CPU']='1'
            else:env.pop('JESTER_BENCHMARK_CPU',None)
            command=[str(binary/'JesterPerformanceTests.exe'),f'--project-path={project}',f'--rhi={backend}',
                '--unattended','-o',str(folder/'tests.txt')+',txt']
            started=time.monotonic()
            with (folder/'engine.log').open('wb') as log:
                process=subprocess.Popen(command,cwd=project,env=env,stdout=log,stderr=subprocess.STDOUT)
                try:code=process.wait(timeout=300)
                except subprocess.TimeoutExpired:process.kill();code=process.wait()
            log=(folder/'engine.log').read_text(encoding='utf-8',errors='replace')
            cases={name:json.loads((folder/(name+'.json')).read_text()) for name in ['simple','heavy-morph','multi-mesh'] if (folder/(name+'.json')).exists()}
            registered=re.findall(r'Registering\s+(\w+)\s+RHI',log)
            errors=len(re.findall(r'Trace::(?:Error|Assert)',log))
            result=dict(backend=backend,scale=scale,exitCode=code,registeredRhi=registered,traceErrors=errors,
                seconds=round(time.monotonic()-started,2),cases=cases,
                passed=code==0 and registered==[backend] and errors==0 and len(cases)==3)
            results['runs'].append(result)
            (output/'results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
            print(json.dumps(dict(backend=backend,scale=scale,passed=result['passed'],cases=len(cases))),flush=True)
    return 0 if results['runs'] and all(run['passed'] for run in results['runs']) else 1

if __name__=='__main__':raise SystemExit(main())
