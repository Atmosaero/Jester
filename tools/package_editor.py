"""Assemble an app-local Windows Jester distribution from an O3DE SDK build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import xml.etree.ElementTree as ET
import zipfile


def imports(path):
    data = path.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'Not a PE image: {path}')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    sections, optional_size = struct.unpack_from('<H12xH', data, pe+6)
    optional = pe+24
    magic = struct.unpack_from('<H', data, optional)[0]
    directory = optional+(112 if magic == 0x20b else 96)
    image_base = struct.unpack_from('<Q' if magic == 0x20b else '<I', data, optional+(24 if magic == 0x20b else 28))[0]
    ranges = []
    for i in range(sections):
        virtual_size, rva, raw_size, offset = struct.unpack_from('<IIII', data, optional+optional_size+i*40+8)
        ranges.append((rva, max(virtual_size, raw_size), offset))
    def raw(rva):
        for start, size, offset in ranges:
            if start <= rva < start+size:
                return offset+rva-start
        raise ValueError(f'Invalid RVA in {path}')
    names = set()
    for index, stride, name_index in [(1,20,3),(13,32,1)]:
        rva, size = struct.unpack_from('<II', data, directory+index*8)
        if not rva:
            continue
        offset = raw(rva)
        while any(data[offset:offset+stride]):
            values = struct.unpack_from('<'+'I'*(stride//4), data, offset)
            name_rva = values[name_index]
            if index == 13 and not values[0]&1:
                name_rva -= image_base
            start = raw(name_rva)
            names.add(data[start:data.index(0,start)].decode('ascii').lower())
            offset += stride
    return names


def runtime_settings(project, engine, output):
    registry = output/'Registry'
    for name in ['application_lifecycle_events.setreg','application_options.setreg',
                 'bootstrap.setreg','fileio.setreg','quality.setreg','streamer.setreg',
                 'profiler.setreg','asset_browser_excludes.setreg']:
        shutil.copy2(engine/'Registry'/name,registry/name)
    engine_data = json.loads((engine/'engine.json').read_text())
    for relative in engine_data['external_subdirectories']:
        gem = engine/relative
        manifest = gem/'gem.json'
        if not manifest.is_file():
            continue
        name = json.loads(manifest.read_text(encoding='utf-8-sig')).get('gem_name','')
        if name not in {'Atom_RHI','Atom_RHI_DX12','Atom_RHI_Vulkan','Atom_RPI',
                        'Atom_Feature_Common','Atom_Bootstrap','AtomToolsFramework',
                        'CommonFeaturesAtom','DiffuseProbeGrid','ImGui','ImguiAtom'}:
            continue
        for path in (gem/'Registry').rglob('*.setreg'):
            if 'assetprocessor' in path.name.lower() or 'assetbuilder' in path.name.lower():
                continue
            relative = path.relative_to(gem/'Registry')
            if 'Platform' in relative.parts and 'Windows' not in relative.parts:
                continue
            destination = registry/relative.parent/(name+'_'+path.name)
            destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(path,destination)
    (output/'Assets/Engine').mkdir(parents=True,exist_ok=True)
    shutil.copy2(engine/'Assets/Engine/exclude.filetag',output/'Assets/Engine/exclude.filetag')
    # AtomToolsFramework expects the asset-browser database schema. Ship an empty
    # metadata database, with no SDK paths, source records or dependency on AP.
    original = sqlite3.connect(f'file:{(project/"Cache/assetdb.sqlite").as_posix()}?mode=ro',uri=True)
    destination = output/'Cache/assetdb.sqlite'
    destination.unlink(missing_ok=True)
    with sqlite3.connect(destination) as database:
        for sql, in original.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"):
            database.execute(sql)
        version = original.execute('SELECT * FROM dbinfo').fetchone()
        database.execute('INSERT INTO dbinfo VALUES (?,?)',version)
        for sql, in original.execute("SELECT sql FROM sqlite_master WHERE type='index' AND sql IS NOT NULL"):
            database.execute(sql)
    original.close()


def asset_payload(project, engine, work):
    assets = project/'Cache/pc'
    paths = ['Assets/Engine/SeedAssetList.seed','Gems/Atom/RPI/Assets/seedList.seed',
             'Gems/Atom/Feature/Common/Assets/seedList.seed','Gems/Atom/Bootstrap/Assets/seedList.seed',
             'Gems/AtomLyIntegration/CommonFeatures/Assets/seedList.seed',
             'Gems/DiffuseProbeGrid/Assets/seedList.seed',
             'Gems/AtomLyIntegration/AtomFont/Assets/seedList.seed',
             'Gems/AtomLyIntegration/AtomViewportDisplayIcons/Assets/seedList.seed']
    rows = []
    seen = set()
    for name in paths:
        for item in json.loads((engine/name).read_text(encoding='utf-8-sig'))['ClassData']:
            hint = item.get('pathHint','').lower()
            if (not hint or hint.startswith('bootstrap.') or hint=='engine_dependencies.xml'
                    or not (assets/hint).is_file() or hint in seen):
                continue
            seen.add(hint)
            rows.append(item)
    seed = work/'runtime.seed'
    seed.write_text(json.dumps({'Type':'JsonSerialization','Version':1,
        'ClassName':'AZStd::vector<SeedInfo, allocator>','ClassData':rows},indent=2)+'\n')
    extra = ['materials/jesterdefault.azmaterial','materials/jesterunlit.azmaterial']
    extra += ['engine_dependencies.xml','materials/reflectionprobe/reflectionprobevisualization.azmaterial',
              'postprocess/default.postfxlayercategories']
    extra += [p.relative_to(assets).as_posix() for p in (assets/'models').glob('*.azmodel')]
    command = [str(engine/'bin/Windows/profile/Default/AssetBundlerBatch.exe'),'assetLists',
               f'--project-path={project}','--platform','pc','--seedListFile',str(seed),
               '--addSeed',','.join(extra),'--assetListFile',str(work/'runtime.assetlist'),'--allowOverwrites']
    with (work/'asset-bundler.log').open('wb') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    log = (work/'asset-bundler.log').read_text(encoding='utf-8',errors='replace')
    if '[ERROR]' in log:
        raise RuntimeError('Asset Bundler reported errors; see '+str(work/'asset-bundler.log'))
    tree = ET.parse(work/'runtime_pc.assetlist')
    files = [e.attrib['value'] for e in tree.iter('Class') if e.attrib.get('field')=='assetRelativePath']
    for name in files:
        if Path(name).is_absolute() or '..' in Path(name).parts:
            raise RuntimeError('Unsafe asset path: '+name)
    (work/'assets.json').write_text(json.dumps(sorted(files),indent=2)+'\n')
    return files


def main():
    source = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root', type=Path, default=source)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--crt-root', type=Path, required=True)
    parser.add_argument('--engine-root', type=Path, default=Path('D:/O3DE'))
    parser.add_argument('--archive', action='store_true', help='Also create a ZIP and SHA-256 file')
    args = parser.parse_args()
    project = args.project_root.resolve()
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('Output must be empty; existing distributions are never overwritten')
    binaries = project/'build/bin/profile'
    assets = project/'Cache/pc'
    assert (binaries/'Jester.exe').is_file() and (assets/'assetcatalog.xml').is_file()
    if b'activeCodePage' not in (binaries/'Jester.exe').read_bytes():
        raise RuntimeError('Rebuild Jester with its UTF-8 Windows manifest before packaging')
    output.mkdir(parents=True, exist_ok=True)
    modules = {
        'Atom_RHI':['Atom_RHI.Private.dll'],
        'Atom_RHI_DX12':['Atom_RHI_DX12.Private.dll'],
        'Atom_RHI_Vulkan':['Atom_RHI_Vulkan.Private.dll'],
        'Atom_RHI_Null':['Atom_RHI_Null.Private.dll'],
        'Atom_RPI':['Atom_RPI.Editor.dll'],
        'Atom_Feature_Common':['Atom_Feature_Common.Editor.dll'],
        'Atom_Bootstrap':['Atom_Bootstrap.dll'],
        'AtomToolsFramework':['AtomToolsFramework.Editor.dll'],
        'CommonFeaturesAtom':['CommonFeaturesAtom.Editor.dll'],
        'DiffuseProbeGrid':['DiffuseProbeGrid.Editor.dll'],
        'ImGui':['ImGui.Editor.dll'],
        'ImguiAtom':['ImguiAtom.dll'],
    }
    registry = {'O3DE':{'Gems':{gem:{'Targets':{Path(name).stem:{'Modules':[name]} for name in names}}
                               for gem,names in modules.items()}}}
    (output/'Registry').mkdir(exist_ok=True)
    (output/'Registry/jester_runtime.setreg').write_text(json.dumps(registry,indent=2)+'\n')
    engine = {'engine_name':'JesterRuntime','version':'2.7.0','file_version':2,'external_subdirectories':[]}
    (output/'engine.json').write_text(json.dumps(engine,indent=2)+'\n')
    project_data = json.loads((source/'project.json').read_text())
    project_data.update(engine='JesterRuntime',external_subdirectories=[],gem_names=[])
    (output/'project.json').write_text(json.dumps(project_data,indent=2)+'\n')
    (output/'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n')
    work = source/'artifacts/packaging'/output.name
    work.mkdir(parents=True,exist_ok=True)
    files = asset_payload(project,args.engine_root,work)
    for name in files+['assetcatalog.xml']:
        destination = output/'Cache/pc'/name
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(assets/name,destination)
    runtime_settings(project,args.engine_root,output)
    shutil.copytree(source/'examples',output/'examples')
    shutil.copytree(source/'licenses',output/'licenses')
    for name in ['LICENSE','THIRD_PARTY.md']:
        shutil.copy2(source/name,output/name)
    shutil.copytree(source/'docs',output/'docs')
    qt = project/'build/packages/packages/qt-6.10.2-rev5-windows/qt'
    (output/'licenses/Qt').mkdir(exist_ok=True)
    for name in ['LICENSE','LGPL-3.0-only.txt','QT-NOTICE.TXT']:
        shutil.copy2(qt/name,output/'licenses/Qt'/name)
    shutil.copy2(source/'licenses/Qt-GPL-3.0.txt',output/'licenses/Qt/LICENSE.GPLv3')
    (output/'licenses/Qt/SOURCE.txt').write_text(
        'Qt 6.10.2, shared libraries, LGPLv3. The supplied SDK selects LGPLv3.\n'
        'Qt DLLs can be replaced by ABI-compatible modified builds.\n'
        'Corresponding Qt source distributions:\n'
        'https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtbase-everywhere-src-6.10.2.tar.xz\n'
        'https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtsvg-everywhere-src-6.10.2.tar.xz\n'
        'SDK package: qt-6.10.2-rev5-windows.\n'
        'Jester source: https://github.com/Atmosaero/Jester\n',encoding='utf-8')
    (output/'licenses/Microsoft-VC-Runtime.txt').write_text(
        'App-local Microsoft Visual C++ runtime, supplied by Visual Studio 2026.\n'
        'Copyright Microsoft Corporation. All rights reserved.\n'
        'Redistributable runtime files are included; installing Visual Studio is not required.\n'
        'https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files\n',encoding='utf-8')
    seeds = [binaries/'Jester.exe']
    if (binaries/'D3D12Core.dll').is_file():
        seeds.append(binaries/'D3D12Core.dll')
    seeds += [binaries/name for names in modules.values() for name in names]
    for folder in ['platforms','imageformats','iconengines']:
        for path in (binaries/folder).glob('*.dll'):
            destination = output/folder/path.name
            destination.parent.mkdir(exist_ok=True)
            shutil.copy2(path,destination)
            seeds.append(path)
    lookup = {p.name.lower():p for p in binaries.glob('*.dll')}
    lookup.update({p.name.lower():p for p in args.crt_root.glob('*.dll')})
    pending = list(seeds)
    copied = set()
    system = set()
    while pending:
        path = pending.pop()
        name = path.name.lower()
        if name in copied:
            continue
        copied.add(name)
        if path.parent == binaries or path.parent == args.crt_root:
            shutil.copy2(path,output/path.name)
        for dependency in imports(path):
            if dependency in lookup:
                pending.append(lookup[dependency])
            elif dependency.startswith(('api-ms-','ext-ms-')) or (Path(os.environ['SystemRoot'])/'System32'/dependency).exists():
                system.add(dependency)
            else:
                raise RuntimeError(f'Missing dependency {dependency} imported by {path.name}')
    report = dict(version=project_data['version'],configuration='profile',sdkDisplayVersion='26.10',
                  symbolsIncluded=False,windowsProcessCodePage='UTF-8',runtimeFiles=sorted(copied),
                  processedAssets=len(files),operatingSystemDependencies=sorted(system),
                  buildInfo=subprocess.check_output([str(binaries/'Jester.exe'),'--build-info']).decode().strip(),
                  executableSha256=hashlib.sha256((output/'Jester.exe').read_bytes()).hexdigest())
    (output/'BUILD-INFO.json').write_text(json.dumps(report,indent=2)+'\n')
    for api in ['DX12','Vulkan']:
        (output/f'Jester {api}.cmd').write_text('@echo off\ncd /d "%~dp0"\nstart "" "%~dp0Jester.exe" --rhi='+api.lower()+' %*\n',encoding='ascii')
    (output/'START-HERE.txt').write_text(
        f'Jester {project_data["version"]} - Windows x64\n\n'
        'Extract the entire folder, then double-click Jester.exe.\n'
        'Jester DX12.cmd / Jester Vulkan.cmd explicitly select the renderer.\n'
        'No O3DE SDK, Qt SDK, Python, Visual Studio or Asset Processor installation is needed.\n'
        'Windows 10 version 1903+ / Windows 11 x64 and a compatible graphics driver are required.\n'
        'Use Scene > Choose scene for the included synthetic examples, or open FBX/GLB/glTF.\n'
        'Save projects in a writable folder. Local settings/logs are created under user/.\n'
        'This is an alpha; experimental physics limits are described in docs/PHYSICS.md.\n'
        'Licenses and source information are in licenses/ and THIRD_PARTY.md.\n',encoding='utf-8')
    forbidden = [p for p in output.rglob('*') if p.is_file() and
                 (p.suffix.lower() in {'.pdb','.lib','.exp','.obj','.py','.pyd','.lock'}
                  or 'python' in p.name.lower() or 'builder' in p.name.lower()
                  or 'qt6test' in p.name.lower() or p.name.endswith('Tests.exe'))]
    if forbidden:
        raise RuntimeError('Development-only payload: '+str(forbidden))
    manifest = {p.relative_to(output).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(output.rglob('*')) if p.is_file()}
    (output/'FILE-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if args.archive:
        archive = output.parent/(output.name+'.zip')
        if archive.exists():
            raise RuntimeError('Archive already exists: '+str(archive))
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as bundle:
            for path in sorted(output.rglob('*')):
                if path.is_file():bundle.write(path,output.name+'/'+path.relative_to(output).as_posix())
        archive.with_suffix('.zip.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
    print(json.dumps(dict(output=str(output),modules=len(modules),runtimeFiles=len(copied),
                         sizeMiB=round(sum(p.stat().st_size for p in output.rglob('*') if p.is_file())/1048576,1))))


if __name__ == '__main__':
    main()
