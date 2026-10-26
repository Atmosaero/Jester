"""Verify a native editor workflow across restart using its local MCP server.

Python standard library only. Tokens remain in the ignored output directory;
reports contain no connection credentials. Also works with a portable folder.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.request


class Session:
    def __init__(self, executable, project, folder, preferences, backend, scale, isolate, allow_signed_host_hooks=False):
        folder.mkdir(parents=True, exist_ok=True)
        self.folder = folder
        self.connection = folder/'connection.json'
        self.connection.unlink(missing_ok=True)
        env = os.environ.copy()
        env.pop('QT_QPA_PLATFORM', None)
        if isolate:
            for key in list(env):
                if key.upper().startswith(('QT_', 'O3DE', 'LY_', 'PYTHON')):
                    del env[key]
            system_root=Path(os.environ['SystemRoot'])
            env['PATH'] = str(system_root/'System32')+';'+str(system_root)
        env.update(QT_SCALE_FACTOR=str(scale), JESTER_PREFERENCES_FILE=str(preferences), JESTER_RHI=backend)
        self.log = (folder/'engine.log').open('wb')
        self.process = subprocess.Popen([str(executable), '--project-path='+str(project), '--rhi='+backend,
            '--mcp', '--mcp-port', '0', '--mcp-config', str(self.connection), '--unattended'],
            cwd=folder, env=env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic()+60
        while not self.connection.exists() and self.process.poll() is None and time.monotonic()<deadline:
            time.sleep(.1)
        if not self.connection.exists():
            self.abort()
            raise RuntimeError('Editor did not create its local MCP connection')
        config = json.loads(self.connection.read_text(encoding='utf-8-sig'))
        config = config.get('mcpServers', config).get('jester', config)
        self.url = config['url']
        self.headers = {**config['headers'], 'Content-Type':'application/json', 'Accept':'application/json, text/event-stream'}
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.serial = 0
        reply = self.rpc('initialize', {'protocolVersion':'2025-11-25', 'capabilities':{},
            'clientInfo':{'name':'JesterWorkflowVerification','version':'1'}})
        self.headers['MCP-Protocol-Version'] = reply['result']['protocolVersion']
        self.rpc('notifications/initialized', notification=True)
        self.backend = backend
        self.executable = executable
        self.isolate = isolate
        self.allow_signed_host_hooks=allow_signed_host_hooks
        self.size_window()

    def rpc(self, method, params=None, notification=False):
        self.serial += 1
        request = {'jsonrpc':'2.0', 'method':method}
        if not notification:
            request['id'] = self.serial
        if params is not None:
            request['params'] = params
        with self.opener.open(urllib.request.Request(self.url, data=json.dumps(request).encode(),
                              headers=self.headers), timeout=60) as response:
            data = response.read()
        return json.loads(data) if data else {}

    def tool(self, name, arguments=None, expected_error=False):
        reply = self.rpc('tools/call', {'name':name,'arguments':arguments or {}})
        failed = 'error' in reply or reply.get('result',{}).get('isError',False)
        if bool(failed)!=expected_error:
            # Tool payloads here contain only synthetic fixture data, no tokens.
            raise AssertionError(f'{name}: unexpected error state: {reply}')
        return reply.get('result',{}).get('structuredContent',{})

    def size_window(self):
        user = ctypes.windll.user32
        user.SetThreadDpiAwarenessContext.argtypes = [wintypes.HANDLE]
        user.SetThreadDpiAwarenessContext(wintypes.HANDLE(-4))
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
        user.GetWindow.argtypes = [wintypes.HWND,wintypes.UINT]
        user.GetWindow.restype = wintypes.HWND
        user.MoveWindow.argtypes = [wintypes.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,wintypes.BOOL]
        windows = []
        callback = ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
        @callback
        def inspect(handle, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(handle,ctypes.byref(pid))
            if pid.value==self.process.pid and user.IsWindowVisible(handle) and not user.GetWindow(handle,4):
                windows.append(handle)
            return True
        user.EnumWindows(inspect,0)
        assert windows, 'Main editor window is missing'
        assert user.MoveWindow(windows[0],40,40,1280,720,True)

    def capture(self, name, target='window'):
        path = self.folder/(name+'.png')
        captured=self.tool('screenshot', {'target':target,'path':str(path),'maxWidth':3840})
        if target=='viewport':
            camera=self.tool('editor_state',{'nodes':False})['camera']
            expected=[round(camera['width']*camera['devicePixelRatio']),round(camera['height']*camera['devicePixelRatio'])]
            # Native window edges can round in opposite directions at 150%.
            assert all(abs(actual-wanted)<=1 for actual,wanted in zip([captured['width'],captured['height']],expected)), ('Native capture and logical viewport differ',captured,expected)
        assert path.stat().st_size>10000
        return path

    def close(self):
        if self.isolate:
            query = '[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false); '+f'(Get-Process -Id {self.process.pid}).Modules | Select-Object -ExpandProperty FileName | ConvertTo-Json'
            raw = subprocess.check_output(['powershell','-NoProfile','-Command',query],encoding='utf-8',errors='replace')
            modules = json.loads(raw)
            if isinstance(modules,str):
                modules = [modules]
            app = self.executable.parent.resolve()
            windows = Path(os.environ['SystemRoot']).resolve()
            external = []
            injected = []
            for name in modules:
                path = Path(name).resolve()
                if app not in path.parents and windows not in path.parents:
                    publisher=None
                    program_data=Path(os.environ['ProgramData']).resolve()
                    if self.allow_signed_host_hooks:
                        if path.name.lower()=='mpoav.dll' and program_data/'Microsoft/Windows Defender/Platform' in path.parents:
                            publisher='O=Microsoft Corporation'
                        elif path==program_data/'obs-studio-hook/graphics-hook64.dll':
                            publisher='CN=Hugh Bailey'
                    if publisher:
                        literal="'"+str(path).replace("'","''")+"'"
                        query='[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false); '+f'$s=Get-AuthenticodeSignature -LiteralPath {literal}; '+"[pscustomobject]@{status=$s.Status.ToString();subject=$s.SignerCertificate.Subject}|ConvertTo-Json"
                        signature=json.loads(subprocess.check_output(['powershell','-NoProfile','-Command',query],encoding='utf-8'))
                        if signature['status']=='Valid' and publisher in signature['subject']:
                            injected.append({'name':path.name,'path':str(path),'publisher':signature['subject'],
                                'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
                            continue
                    external.append(str(path))
            (self.folder/'loaded-modules.json').write_text(json.dumps({'modules':modules,'external':external,'signedHostInjections':injected},indent=2),encoding='utf-8')
            assert not external, 'A runtime module resolved outside the portable folder and Windows; inspect loaded-modules.json'
        try:
            self.tool('editor_action', {'id':'quit'})
        except (OSError,ValueError):
            pass
        code = self.process.wait(timeout=20)
        self.log.close()
        text = (self.folder/'engine.log').read_text(encoding='utf-8',errors='replace')
        errors = len(re.findall(r'Trace::(?:Error|Assert)',text))
        registered = re.findall(r'Registering\s+(\w+)\s+RHI',text)
        assert code==0 and errors==0 and registered==[self.backend], (code,errors,registered)
        return {'exitCode':code,'traceErrors':errors,'registeredRhi':registered,
                'executableSha256':hashlib.sha256(self.executable.read_bytes()).hexdigest(),
                **({'moduleAuditPassed':True,'signedHostInjections':[item['name'] for item in injected]} if self.isolate else {})}

    def abort(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=15)
        self.log.close()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root',type=Path,default=root)
    parser.add_argument('--exe',type=Path,default=root/'build/bin/profile/Jester.exe')
    parser.add_argument('--output-dir',type=Path,default=root/'artifacts/editor-workflow')
    parser.add_argument('--backends',nargs='+',choices=['dx12','vulkan'],default=['dx12','vulkan'])
    parser.add_argument('--scales',nargs='+',type=float,default=[1,1.5])
    parser.add_argument('--languages',nargs='+',choices=['en','ru'],default=['en','ru'])
    parser.add_argument('--isolate',action='store_true')
    parser.add_argument('--allow-signed-host-hooks',action='store_true',help='Separately record valid signed Defender/OBS hooks at their exact standard paths; reject other external DLLs')
    args = parser.parse_args()
    project,executable,output = args.project_root.resolve(),args.exe.resolve(),args.output_dir.resolve()
    output.mkdir(parents=True,exist_ok=True)
    results = []
    for backend in args.backends:
        for scale in args.scales:
            for language in args.languages:
                folder = output/backend/str(scale)/language
                folder.mkdir(parents=True,exist_ok=True)
                preferences = folder/'preferences.json'
                preferences.write_text(json.dumps({'format':'JesterPreferences','version':1,'language':language}),encoding='utf-8')
                session = None
                try:
                    session = Session(executable,project,folder/'before',preferences,backend,scale,args.isolate,args.allow_signed_host_hooks)
                    session.tool('scene',{'operation':'open','path':str(project/'examples/welcome.jst')})
                    session.tool('scene',{'operation':'rename','name':'Jester studio'})
                    session.tool('pose_transform',{'node':7,'rotation':[0,0,25],'frame':15,'key':True})
                    unicode_folder = folder/'Проекты Jester'
                    unicode_folder.mkdir(exist_ok=True)
                    path = unicode_folder/'Анимация персонажа.JST'
                    active = session.tool('scene',{'operation':'save','path':str(path)})['activeScene']
                    authored = json.loads(path.read_text(encoding='utf-8'))
                    session.tool('history',{'operation':'undo'})
                    session.tool('history',{'operation':'redo'})
                    session.tool('scene',{'operation':'new','template':'mannequin'})
                    session.tool('scene',{'operation':'rename','name':'Secondary character'})
                    session.tool('scene',{'operation':'save','path':str(folder/'secondary.jst')})
                    session.tool('scene',{'operation':'switch','index':active})
                    session.tool('editor_action',{'id':'toggleSceneSettings','checked':False})
                    session.tool('editor_action',{'id':'toggleSceneSettings','checked':True})
                    session.capture('native-viewport','viewport')
                    deadline=time.monotonic()+5
                    renderer=session.tool('scene_visuals',{'operation':'get'})['renderer']
                    while renderer.get('gpuMeshes',0)<1 and time.monotonic()<deadline:
                        time.sleep(.05)
                        renderer=session.tool('scene_visuals',{'operation':'get'})['renderer']
                    assert renderer['rhi']==backend and renderer['skinningPath']=='gpu' and renderer['gpuMeshes']>0, renderer
                    for mode in ['lit','unlit','wireframe']:
                        session.tool('scene_visuals',{'operation':'set','settings':{'mode':mode}})
                        session.capture(mode)
                    session.tool('scene_visuals',{'operation':'preset','preset':'studio'})
                    settings = {'mode':'lit','lighting':{'main':{'azimuth':65,'intensityLux':2400}},
                        'environment':{'rotation':35,'exposureEV':.5},
                        'post':{'enabled':True,'exposureEV':-8,'saturation':.85,'contrast':1.1,
                                'bloom':{'enabled':True,'threshold':1,'intensity':.2}}}
                    session.tool('scene_visuals',{'operation':'set','settings':settings})
                    configured = session.tool('scene_visuals',{'operation':'get'})
                    session.tool('history',{'operation':'undo'})
                    session.tool('history',{'operation':'redo'})
                    assert session.tool('scene_visuals',{'operation':'get'})['authored']==configured['authored']
                    unsupported=session.tool('scene_visuals',{'operation':'set','settings':{'lighting':{'fill':{'shadows':True}}}},expected_error=True)
                    assert unsupported['code']=='visual.unsupported_parameter'
                    unknown=session.tool('scene_visuals',{'operation':'set','settings':{'doesNotExist':True}},expected_error=True)
                    assert unknown['code']=='visual.invalid_request'
                    invalid=session.tool('editor_preferences',{'operation':'set','settings':{'appearance':{'theme':'invalid'}}},expected_error=True)
                    assert invalid['code']=='preferences.invalid_appearance'
                    assert session.tool('scene_visuals',{'operation':'get'})['authored']==configured['authored']
                    assert session.tool('scene_visuals',{'operation':'capabilities'})
                    saved_preferences = session.tool('editor_preferences',{'operation':'set','settings':{'language':language,
                        'appearance':{'fontFamily':'Arial','fontSize':10.5,'uiScale':1.05,'theme':'dark'}}})
                    session.tool('scene',{'operation':'save','path':str(path)})
                    saved = json.loads(path.read_text(encoding='utf-8'))
                    for field in ['keys','curves','layers','contacts','annotations','meshes']:
                        assert saved[field]==authored[field], field
                    session.capture('scene-settings')
                    session.tool('record_frames',{'directory':str(folder/'sequence'),'first':0,'last':2,
                        'target':'viewport','maxWidth':640,'overwrite':True})
                    assert len(list((folder/'sequence').glob('*.png')))==3
                    before = session.close()
                    session = Session(executable,project,folder/'after',preferences,backend,scale,args.isolate,args.allow_signed_host_hooks)
                    restored_preferences = session.tool('editor_preferences',{'operation':'get'})
                    assert restored_preferences['preferences']==saved_preferences['preferences']
                    assert restored_preferences['language']==language
                    session.tool('scene',{'operation':'open','path':str(path)})
                    restored = session.tool('scene_visuals',{'operation':'get'})
                    assert restored['authored']==configured['authored']
                    session.capture('restored')
                    # Save again to compare all authored motion after a process restart.
                    again = unicode_folder/'reopened.jst'
                    session.tool('scene',{'operation':'save','path':str(again)})
                    reopened = json.loads(again.read_text(encoding='utf-8'))
                    for field in ['keys','curves','layers','contacts','annotations','meshes','visuals']:
                        assert reopened[field]==saved[field],field
                    session.tool('editor_action',{'id':'about_jester'})
                    deadline=time.monotonic()+5
                    modal=''
                    while not modal and time.monotonic()<deadline:
                        modal=session.tool('ui_inspect').get('modal','')
                        if not modal:
                            time.sleep(.05)
                    assert modal, 'About dialog did not open'
                    session.capture('about-atom-rhi')
                    session.tool('ui_click',{'id':modal,'operation':'accept'})
                    after = session.close()
                    result = {'backend':backend,'scale':scale,'language':language,'passed':True,
                        'physicalWindow':[1280,720],'before':before,'after':after,
                        'unicodeProject':True,'motionPreserved':True,'preferenceRestart':True,'sequenceFrames':3,
                        'nativeGpuSkinning':True}
                finally:
                    if session is not None and session.process.poll() is None:
                        session.abort()
                results.append(result)
                (output/'results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
                print(backend,scale,language,'PASS',flush=True)


if __name__=='__main__':
    main()
