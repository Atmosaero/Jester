"""Exercise a workspace Jester window with real Windows input and native MCP.

The connection file stays local; credentials are never printed or put in argv.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('--pid', type=int, required=True)
parser.add_argument('--config', default='artifacts/live-mcp.json')
parser.add_argument('--import-file', default='examples/welcome.glb', help='Character to check at the end of the workflow')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]

def invoke(name, arguments=None):
    result = subprocess.run([sys.executable, '-X', 'utf8', str(root/'scripts/jester-mcp.py'),
        '--config', str(root/args.config), '--call', name, '--arguments', json.dumps(arguments or {})],
        cwd=root, capture_output=True, text=True, encoding='utf-8', timeout=90)
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.stderr[:300]}')
    response = json.loads(result.stdout)
    return response['result']['structuredContent']

user = ctypes.windll.user32
user.GetWindow.argtypes=[wintypes.HWND,wintypes.UINT]
user.GetWindow.restype=wintypes.HWND
user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
user.GetWindowTextW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
user.SetForegroundWindow.argtypes=[wintypes.HWND]
user.ClientToScreen.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.POINT)]
class MouseInput(ctypes.Structure):
    _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG), ('data', wintypes.DWORD),
                ('flags', wintypes.DWORD), ('time', wintypes.DWORD), ('extra', wintypes.WPARAM)]
class KeyInput(ctypes.Structure):
    _fields_ = [('key', wintypes.WORD), ('scan', wintypes.WORD), ('flags', wintypes.DWORD),
                ('time', wintypes.DWORD), ('extra', wintypes.WPARAM)]
class InputUnion(ctypes.Union):
    _fields_ = [('mouse', MouseInput), ('keyboard', KeyInput)]
class Input(ctypes.Structure):
    _fields_ = [('type', wintypes.DWORD), ('value', InputUnion)]
user.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]

def keys(events):
    inputs = (Input*len(events))()
    for item, (key, scan, flags) in zip(inputs, events):
        item.type = 1
        item.value.keyboard = KeyInput(key, scan, flags, 0, 0)
    assert user.SendInput(len(inputs), inputs, ctypes.sizeof(Input)) == len(inputs)
    time.sleep(.25)

user.SetThreadDpiAwarenessContext.argtypes = [wintypes.HANDLE]
user.SetThreadDpiAwarenessContext(wintypes.HANDLE(-4))
windows = []
callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
@callback_type
def inspect(window, _):
    process = wintypes.DWORD()
    user.GetWindowThreadProcessId(window, ctypes.byref(process))
    if process.value == args.pid and user.IsWindowVisible(window):
        title = ctypes.create_unicode_buffer(512)
        user.GetWindowTextW(window, title, len(title))
        if title.value.endswith('Jester') and not user.GetWindow(window,4):
            windows.append(window)
    return True
user.EnumWindows(inspect, 0)
assert len(windows) == 1, 'Expected the workspace Jester main window'
window = windows[0]
user.SetForegroundWindow(window)
origin = wintypes.POINT()
assert user.ClientToScreen(window, ctypes.byref(origin))
controls = invoke('ui_inspect')['controls']
viewport = next(c for c in controls if c['objectName'] == 'viewport')

def drag(button_down, button_up, start, delta):
    x = origin.x + viewport['x'] + round(start[0])
    y = origin.y + viewport['y'] + round(start[1])
    user.SetCursorPos(x, y)
    time.sleep(.1)
    user.mouse_event(button_down, 0, 0, 0, 0)
    for step in range(1, 13):
        user.SetCursorPos(x + round(delta[0]*step/12), y + round(delta[1]*step/12))
        time.sleep(.025)
    user.mouse_event(button_up, 0, 0, 0, 0)
    time.sleep(.25)

before = invoke('editor_state', {'nodes': False})
center = (viewport['width']*.6, viewport['height']*.4)
drag(0x0008, 0x0010, center, (65, 25))
orbit = invoke('editor_state', {'nodes': False})
assert orbit['camera']['yaw'] != before['camera']['yaw'], 'Native right drag did not orbit'
drag(0x0020, 0x0040, center, (30, 15))
pan = invoke('editor_state', {'nodes': False})
assert pan['camera']['center'] != orbit['camera']['center'], 'Native middle drag did not pan'
user.mouse_event(0x0800, 0, 0, 120, 0)
time.sleep(.25)
zoom = invoke('editor_state')
assert zoom['camera']['distance'] < pan['camera']['distance'], 'Native wheel did not zoom'
hips = next(node for node in zoom['nodes'] if node['id'] == 0)
drag(0x0002, 0x0004, hips['screen'], (14, -12))
posed = invoke('editor_state')
assert posed['selected'] == 0
assert next(node for node in posed['nodes'] if node['id'] == 0)['worldPosition'] != hips['worldPosition'], 'Native posing drag did not edit the rig'
invoke('editor_action', {'id': 'addNoteAction'})
time.sleep(.3)
text_control = next(c for c in invoke('ui_inspect')['controls'] if c['objectName'] == 'noteText' and c['visible'])
(root/'artifacts/native-note-control.json').write_text(json.dumps(text_control, indent=2))
invoke('screenshot', {'target': 'window', 'path': str(root/'artifacts/live-note-before.png')})
user.SetCursorPos(origin.x+text_control['x']+text_control['width']//2,
                  origin.y+text_control['y']+text_control['height']//2)
user.mouse_event(0x0002, 0, 0, 0, 0)
user.mouse_event(0x0004, 0, 0, 0, 0)
time.sleep(.2)
assert any(c['objectName'] == 'noteText' and c['visible'] for c in invoke('ui_inspect')['controls']), 'Native click closed the note card'
note_text = 'Native note input — проверка'
keys([(0, ord(char), flag) for char in note_text for flag in (4, 6)])
typed = next(c for c in invoke('ui_inspect')['controls'] if c['objectName'] == 'noteText' and c['visible'])
assert typed['text'] == note_text, 'Native note keyboard input did not reach the card'
assert invoke('editor_state', {'nodes': False})['frame'] == posed['frame'], 'Typing in a note triggered animation shortcuts'
invoke('screenshot', {'target': 'window', 'path': str(root/'artifacts/live-note-card.png')})
subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    str(root/'tools/capture_live_window.ps1'), '-Path', str(root/'artifacts/live-note-desktop.png'), '-NoActivate'],
    cwd=root, check=True, capture_output=True)
keys([(0x11, 0, 0), (0x0D, 0, 0), (0x0D, 0, 2), (0x11, 0, 2)])
annotations = invoke('motion_annotations', {'operation': 'list', 'detail': 'full'})['annotations']
assert any(note_text in json.dumps(note, ensure_ascii=False) for note in annotations), 'Ctrl+Enter did not save the native note'
project = root/'artifacts/live-smoke.jst'
invoke('scene', {'operation': 'save', 'path': str(project)})
assert project.is_file()
invoke('screenshot', {'target': 'window', 'path': str(root/'artifacts/live-smoke.png')})
frames = root/'artifacts/native-frames'
invoke('record_frames', {'directory': str(frames), 'first': 0, 'last': 2,
    'target': 'viewport', 'maxWidth': 640, 'overwrite': True})
assert len(list(frames.glob('*.png'))) == 3, 'Native frame recording did not produce three images'
saved = invoke('editor_state', {'nodes': False})
invoke('scene', {'operation': 'close', 'index': saved['activeScene']})
reopened = invoke('scene', {'operation': 'open', 'path': str(project)})
assert reopened['nodeCount'] == saved['nodeCount'], 'Saved project node count did not survive reopening'
reopened_notes = invoke('motion_annotations', {'operation': 'list', 'detail': 'full'})['annotations']
assert any(note_text in json.dumps(note, ensure_ascii=False) for note in reopened_notes), 'Native note did not survive reopening'
invoke('scene', {'operation': 'open', 'path': str((root/args.import_file).resolve())})
helmet = invoke('editor_state', {'nodes': False})
assert helmet['nodeCount'] > 0
invoke('screenshot', {'target': 'viewport', 'path': str(root/'artifacts/live-textured-import.png')})
invoke('scene', {'operation': 'save', 'path': str(root/'artifacts/helmet-smoke.jst')})
invoke('scene', {'operation': 'open', 'path': str(project)})
report = {'nativeOrbit': True, 'nativePan': True, 'nativeZoom': True, 'nativePoseDrag': True,
    'nativeNoteMouse': True, 'nativeNoteKeyboard': True, 'nativeNoteSave': True,
    'recordedFrames': 3, 'projectReopen': True, 'texturedImport': True,
    'save': str(project), 'frame': posed['frame'], 'nodes': posed['nodeCount'], 'passed': True}
(root/'artifacts/live-editor-smoke.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, ensure_ascii=False))
