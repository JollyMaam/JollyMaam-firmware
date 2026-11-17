#!/usr/bin/env python3
"""Exercise launcher process/lock cleanup without booting Android or using USB."""
import json
import os
import pty
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

MOCK = r'''
import json, os, signal, socket, subprocess, sys, time
from pathlib import Path
root = Path(os.environ['PADTOOL_TEST_ROOT'])
name = Path(sys.argv[0]).name
args = sys.argv[1:]
def record(**values):
    with (root/'processes').open('a') as f:
        f.write(json.dumps(dict(name=name, pid=os.getpid(), **values))+'\n')
def sleep_with_child():
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])
    record(child=child.pid, qt=os.environ.get('QT_QPA_PLATFORM'), tmp=os.environ.get('TMPDIR'), args=args,
           adb=os.environ.get('ADB'), socket=os.environ.get('ADB_SERVER_SOCKET'))
    while True:
        if name == 'scrcpy' and (root/'close-window').exists(): sys.exit(0)
        time.sleep(.05)
if name == 'gradlew':
    if os.environ.get('PADTOOL_TEST_BUILD_OK'):
        record(event='build'); sys.exit(0)
    sleep_with_child()
elif name == 'avdmanager':
    config = Path(os.environ['ANDROID_AVD_HOME'])/'cyclone-padtool-api35.avd'/'config.ini'
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('')
elif name == 'emulator':
    if '-accel-check' in args: sys.exit(0)
    if os.environ.get('PADTOOL_TEST_FAIL'):
        print('Test Qt platform startup failure', flush=True)
        sys.exit(23)
    (root/'emulator.pid').write_text(str(os.getpid()))
    sleep_with_child()
elif name == 'scrcpy':
    if os.environ.get('PADTOOL_TEST_SCRCPY_FAIL'):
        record(event='failed')
        print('Test scrcpy startup failure', flush=True)
        sys.exit(23)
    sleep_with_child()
elif name == 'adb':
    if args[-2:] == ['server', 'nodaemon']:
        record()
        port=int(os.environ['ANDROID_ADB_SERVER_PORT'])
        with socket.socket() as s:
            s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            s.bind(('127.0.0.1',port)); s.listen()
            while True:
                conn,_=s.accept(); conn.close()
    elif args == ['devices']: print('List of devices attached')
    elif args[-2:] == ['getprop','sys.boot_completed']:
        print('1' if (root/'emulator.pid').exists() else '')
    elif args[-2:] == ['emu','kill']:
        try: os.kill(int((root/'emulator.pid').read_text()),signal.SIGTERM)
        except (FileNotFoundError,ProcessLookupError): pass
    elif 'am' in args and 'start' in args:
        record(event='launch'); (root/'ready').touch()
'''


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().split(') ')[1][0] != 'Z'
    except FileNotFoundError:
        return False


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='padtool-lifecycle-')
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.bin = self.root/'bin'; self.bin.mkdir()
        self.android = self.root/'android'; self.android.mkdir()
        source = Path(__file__).resolve().parents[1]/'run-emulator'
        shutil.copyfile(source, self.android/'run-emulator')
        (self.android/'tools').mkdir()
        shutil.copyfile(source.parent/'tools/launcher_lock.py', self.android/'tools/launcher_lock.py')
        apk = self.root/'build/android/app/outputs/apk/debug/app-debug.apk'
        apk.parent.mkdir(parents=True); apk.write_bytes(b'test')
        for name in ('adb','emulator','scrcpy','avdmanager','gradlew'):
            file = (self.android if name == 'gradlew' else self.bin)/name
            file.write_text(f'#!{sys.executable}\n'+MOCK); file.chmod(0o755)
        with socket.socket() as s:
            s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
        self.env = dict(os.environ, JOLLYMAAM_EMULATOR_SHELL="1", PATH=str(self.bin)+':'+os.environ['PATH'],
                        XDG_STATE_HOME=str(self.root/'state'), QT_QPA_PLATFORM='xcb',
                        PADTOOL_TEST_ROOT=str(self.root), PADTOOL_ADB_PORT=str(port))
        self.processes = []
        self.addCleanup(self.stop_remaining)

    def start(self, build=False, fail=False, terminal=False, headless=False, scrcpy=False):
        for name in ('ready','emulator.pid','close-window'):
            (self.root/name).unlink(missing_ok=True)
        output=(self.root/f'output-{len(self.processes)}').open('w+')
        self.addCleanup(output.close)
        stdin=None
        if terminal:
            self.terminal_input, stdin=pty.openpty()
            self.addCleanup(os.close, self.terminal_input)
            self.addCleanup(os.close, stdin)
        p=subprocess.Popen(['bash',str(self.android/'run-emulator'),'--software']+([] if build else ['--no-build'])+(['--headless'] if headless else [])+(['--scrcpy'] if scrcpy else []),
                           env=dict(self.env, **({'PADTOOL_TEST_FAIL':'1'} if fail else {})),
                           stdin=stdin,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
        self.processes.append(p)
        return p,output

    def wait_for(self, condition):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            if condition(): return
            time.sleep(.05)
        self.fail('Timed out waiting for launcher state')

    def records(self):
        try: return [json.loads(s) for s in (self.root/'processes').read_text().splitlines()]
        except FileNotFoundError: return []

    def stop_remaining(self):
        for p in self.processes:
            if p.poll() is None:
                os.killpg(p.pid,signal.SIGTERM)
                try: p.wait(timeout=10)
                except subprocess.TimeoutExpired: os.killpg(p.pid,signal.SIGKILL); p.wait()
        for row in self.records():
            for pid in (row['pid'],row.get('child')):
                if pid and alive(pid):
                    try: os.kill(pid,signal.SIGKILL)
                    except ProcessLookupError: pass

    def assert_clean(self):
        self.wait_for(lambda: not any(alive(pid) for r in self.records() for pid in (r['pid'],r.get('child')) if pid))
        state=self.root/'state/cyclone-padtool/emulator'
        self.wait_for(lambda: not list(state.glob('run.*')))
        lock=state/'launch.lock'
        result=subprocess.run(['flock','-n',str(lock),'true'])
        self.assertEqual(0,result.returncode,'Launcher lock leaked')

    def test_ctrl_c_stops_emulator_adb_and_descendants_and_releases_lock(self):
        p,_=self.start(scrcpy=True)
        self.wait_for(lambda:(self.root/'ready').exists())
        self.assertIsNone(p.poll(),'Launcher must remain in foreground')
        emulator=next(r for r in self.records() if r['name']=='emulator')
        self.assertEqual('xcb',emulator['qt'])
        args=emulator['args']
        self.assertIn('-no-window', args)
        features=[args[i+1] for i, arg in enumerate(args) if arg == '-feature']
        self.assertEqual(['VirtioTablet', '-VirtioMouse', '-VirtioDualModeMouse'], features)
        self.assertTrue(Path(emulator['tmp']).is_dir())
        self.wait_for(lambda:any(r['name']=='scrcpy' for r in self.records()))
        viewer=next(r for r in self.records() if r['name']=='scrcpy')
        self.assertEqual(['--serial','emulator-5580','--no-audio','--mouse=sdk','--window-title','Cyclone padtool'],viewer['args'])
        self.assertEqual(str(self.bin/'adb'),viewer['adb'])
        self.assertEqual('tcp:127.0.0.1:'+self.env['PADTOOL_ADB_PORT'],viewer['socket'])
        self.assertEqual(emulator['tmp'],viewer['tmp'])
        os.killpg(p.pid,signal.SIGINT)
        p.wait(timeout=10)
        self.assertFalse(list((self.root/'state/cyclone-padtool/emulator').glob('run.*')), 'Launcher returned before cleanup')
        self.assert_clean()
        self.assertTrue((self.root/'state/cyclone-padtool/emulator/avd/cyclone-padtool-api35.avd/config.ini').exists())

    def test_ctrl_c_during_build_stops_java_like_descendants(self):
        p,_=self.start(build=True)
        self.wait_for(lambda:any(r['name']=='gradlew' for r in self.records()))
        os.killpg(p.pid,signal.SIGINT)
        p.wait(timeout=10)
        self.assertFalse(list((self.root/'state/cyclone-padtool/emulator').glob('run.*')), 'Launcher returned before cleanup')
        self.assert_clean()

    def test_failed_qt_startup_cleans_up_and_allows_retry(self):
        p,output=self.start(fail=True)
        self.assertNotEqual(0,p.wait(timeout=10))
        output.seek(0); self.assertIn('Test Qt platform startup failure',output.read())
        self.assert_clean()
        p,_=self.start()
        self.wait_for(lambda:(self.root/'ready').exists())
        os.killpg(p.pid,signal.SIGINT)
        p.wait(timeout=10)
        self.assertFalse(list((self.root/'state/cyclone-padtool/emulator').glob('run.*')), 'Launcher returned before cleanup')
        self.assert_clean()

    def test_terminal_reload_keeps_emulator_and_quit_cleans_up(self):
        self.env['PADTOOL_TEST_BUILD_OK']='1'
        p,_=self.start(terminal=True, scrcpy=True)
        self.wait_for(lambda:(self.root/'ready').exists())
        os.write(self.terminal_input,b'r\n')
        self.wait_for(lambda:sum(r.get('event')=='launch' for r in self.records())==2)
        self.assertEqual(1,sum(r['name']=='emulator' for r in self.records()))
        self.assertEqual(1,sum(r['name']=='scrcpy' for r in self.records()))
        os.write(self.terminal_input,b'q\n')
        self.assertEqual(0,p.wait(timeout=10))
        self.assertFalse(list((self.root/'state/cyclone-padtool/emulator').glob('run.*')))
        self.assert_clean()

    def test_closing_scrcpy_stops_session_and_descendants(self):
        for terminal in (False, True):
            with self.subTest(terminal=terminal):
                p,_=self.start(terminal=terminal, scrcpy=True)
                self.wait_for(lambda:any(r['name']=='scrcpy' and alive(r['pid']) for r in self.records()))
                (self.root/'close-window').touch()
                self.assertEqual(0,p.wait(timeout=10))
                self.assert_clean()

    def test_failed_scrcpy_stops_emulator_and_adb(self):
        self.env['PADTOOL_TEST_SCRCPY_FAIL']='1'
        p,_=self.start(scrcpy=True)
        self.assertEqual(23,p.wait(timeout=10))
        log=self.root/'state/cyclone-padtool/emulator/scrcpy.log'
        self.assertIn('Test scrcpy startup failure',log.read_text())
        self.assert_clean()

    def test_headless_skips_scrcpy_and_still_uses_no_window(self):
        p,_=self.start(headless=True)
        self.wait_for(lambda:(self.root/'ready').exists())
        self.assertFalse(any(r['name']=='scrcpy' for r in self.records()))
        self.assertIn('-no-window',next(r for r in self.records() if r['name']=='emulator')['args'])
        os.killpg(p.pid,signal.SIGINT); p.wait(timeout=10)
        self.assert_clean()

    def test_default_uses_emulator_window_and_forces_xcb_without_scrcpy(self):
        self.env['QT_QPA_PLATFORM']='wayland'
        self.env.pop('DISPLAY', None)
        p,_=self.start(terminal=True)
        self.wait_for(lambda:(self.root/'ready').exists())
        emulator=next(r for r in self.records() if r['name']=='emulator')
        self.assertNotIn('-no-window', emulator['args'])
        self.assertEqual('xcb', emulator['qt'])
        self.assertFalse(any(r['name']=='scrcpy' for r in self.records()))
        os.write(self.terminal_input,b'q\n')
        self.assertEqual(0,p.wait(timeout=10))
        self.assert_clean()

    def test_headless_and_scrcpy_are_mutually_exclusive(self):
        p,output=self.start(headless=True, scrcpy=True)
        self.assertNotEqual(0,p.wait(timeout=10))
        output.seek(0)
        self.assertIn('--headless and --scrcpy cannot be used together',output.read())
        self.assertEqual([],self.records())


if __name__ == '__main__': unittest.main(verbosity=2)
