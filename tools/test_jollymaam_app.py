import os
import hashlib
import json
import zipfile
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import test_firmware_package as package_tests

STUBS = r'''
#include "src/lib/transport_custom.h"
#include "src/lib/transport_hid.h"
#include "src/lib/transport_jieli.h"
#include "tools/jollymaam_update.h"
#include "src/lib/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int jm_hid_custom_devices(jm_custom_visitor visit,void *user) {
 if(getenv("NO_DEVICES")) return 0;
 struct jm_identity id={0}; strcpy(id.product,"U2C_WIRELESS"); strcpy(id.build,"test-build");
 visit(user,"fake-custom",&id); return 1;
}
int jm_hid_enumerate_8bitdo(jm_hid_visit visit,void *user) { if(getenv("NO_DEVICES")) return 0;visit(user,"fake-boot",0x2dc8,0x3208); return 1; }
int jm_hid_8bitdo_model(const char *p,uint16_t *m) { *m=0x301b; return !strcmp(p,"fake-boot"); }
int jm_jieli_candidates(jm_jieli_candidate_visitor v,void *u) { if(getenv("NO_DEVICES")) return 0;v(u,"fake-jieli",JM_JIELI_BR23);return 1; }
int jm_hid_custom_update(const char *p,const uint8_t expected_uid[16],const uint8_t *d,size_t n,void (*cb)(void*,size_t,size_t),void *u) {
 (void)d; (void)expected_uid; cb(u,n,n); printf("MOCK-CUSTOM:%s\n",p); return getenv("FAIL_CUSTOM") ? -4 : 0;
}
int jm_hid_upgrade_8bitdo(const char *p,const uint8_t *d,size_t n,void (*cb)(void*,unsigned,unsigned),void *u) {
 (void)d;(void)n;cb(u,1,1);printf("MOCK-BOOT:%s\n",p);return 0;
}
int jm_jieli_upgrade(const char *p,const uint8_t *l,size_t s,const uint8_t *d,size_t n) {
 (void)p;(void)l;(void)s;(void)d;(void)n;puts("UNEXPECTED-JIELI");return -1;
}
struct jm_platform_connection { const char *path; enum jm_session_kind kind; };
static struct jm_platform_connection connection;
struct jm_platform_connection *jm_platform_open(const char *path,enum jm_session_kind kind) {
 connection.path=path;connection.kind=kind;return &connection;
}
void jm_platform_close(struct jm_platform_connection *p) { (void)p; }
static int probe_scsi(void *c,const uint8_t *cmd,size_t n,enum jm_scsi_direction dir,uint8_t *data,size_t size,unsigned timeout,struct jm_scsi_result *r) {
 struct jm_platform_connection *p=c;(void)timeout;
 if(strcmp(p->path,"fake-sg") || p->kind!=JM_SESSION_JIELI || n!=6 || cmd[0]!=0x12 || dir!=JM_SCSI_IN || size!=36) return -1;
 memset(r,0,sizeof *r);r->transferred=36;memset(data,0,size);memcpy(data+8,"BR23",4);memcpy(data+16,"UBOOT1.00",9);return 0;
}
const struct jm_session_io *jm_platform_io(void) { static const struct jm_session_io io={.scsi=probe_scsi};return &io; }
int jm_session_run(const struct jm_firmware_target *t,enum jm_session_kind kind,const uint8_t uid[16],
                    const struct jm_session_io *io,void *context,jm_session_progress cb,void *u) {
 (void)io;struct jm_platform_connection *p=context;
 if(kind==JM_SESSION_CUSTOM) return jm_hid_custom_update(p->path,uid,t->update.data,t->update.size,cb,u);
 if(kind==JM_SESSION_8BITDO) { printf("MOCK-BOOT:%s\\n",p->path);return 0; }
 printf("MOCK-JIELI:%s\n",p->path);return 0;
}
int jm_session_identify(const struct jm_session_io *io,void *c,struct jm_identity *id) {
 (void)io;(void)c;memset(id,0,sizeof *id);strcpy(id->product,"U2C_WIRELESS");id->uid[0]=42;return 1;
}
int jm_session_stock_loader(const struct jm_session_io *io,void *c,const uint8_t uid[16]) {
 (void)io;(void)c;if(uid[0]!=42) return -2;puts("MOCK-STOCK-LOADER");return 0;
}
int jollymaam_jieli_command(const char *command) { (void)command;return 0; }
int jollymaam_jieli_app(int argc,char **argv) { (void)argc;(void)argv;return 2; }
int jollymaam_restore_app(int argc,char **argv,int jieli) { (void)argc;(void)argv;(void)jieli;return 2; }
int main(int argc,char **argv) { return jollymaam_update_command(argc-1,argv+1); }
'''


class AppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        stub = cls.root / 'stub.c'
        stub.write_text(STUBS)
        cls.binary = cls.root / 'app'
        sources = ['tools/jollymaam_update.c', 'src/lib/package.c', 'src/lib/manifest.c',
                   'src/lib/sha256.c', 'src/lib/proto_jollymaam.c', 'src/lib/proto_8bitdo.c',
                   'src/lib/jieli_image/entry.c', 'src/lib/jieli_image/crypto.c']
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I.', str(stub), *sources, '-o', str(cls.binary)], check=True)
        scan_stub = cls.root / 'scan-stub.c'
        scan_stub.write_text(STUBS[:STUBS.index('int main(int argc,char **argv)')])
        cls.cli = cls.root / 'cli'
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I.', str(scan_stub), 'tools/jollymaam.c', *sources,
                        'src/lib/jieli_image/image.c', 'src/lib/jieli_image/bankcb.c',
                        'src/lib/jieli_image/key.c', 'src/lib/jieli_image/ufw.c', '-o', str(cls.cli)], check=True)
        cls.package = cls.root / 'firmware.zip'
        cls.package.write_bytes(package_tests.PackageTests().manifest_fixture())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_app(self, commands):
        return subprocess.run([str(self.binary), str(self.package)], input=commands,
                              capture_output=True, text=True, timeout=5)

    def test_stock_loader_requires_explicit_confirmation(self):
        result = subprocess.run([str(self.cli), 'enter-loader', '--yes', 'fake-custom'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn('MOCK-STOCK-LOADER', result.stdout)
        result = subprocess.run([str(self.cli), 'enter-loader', 'fake-custom'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn('MOCK-STOCK-LOADER', result.stdout)

    def test_8bitdo_scan_is_scoped_to_its_bootloaders(self):
        result = subprocess.run([str(self.cli), '8bitdo', 'scan'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn('fake-boot', result.stdout)
        self.assertNotIn('fake-custom', result.stdout)
        self.assertNotIn('fake-jieli', result.stdout)

    def test_scan_includes_custom_devices(self):
        result = subprocess.run([str(self.cli), 'scan'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn('U2C_WIRELESS our firmware build=test-build (fake-custom)', result.stdout)
        self.assertIn('fake-boot', result.stdout)
        self.assertIn('fake-jieli', result.stdout)

    def test_scan_explains_empty_result(self):
        result = subprocess.run([str(self.cli), 'scan'], capture_output=True, text=True, timeout=5,
                                env={**os.environ, 'NO_DEVICES': '1'})
        self.assertEqual(result.returncode, 0)
        self.assertIn('No supported devices found', result.stdout)

    def test_cancel_eof_and_rescan(self):
        for commands in ('', 'q\n', '1\n\nq\n', '1\nno\nq\n', 'r\nq\n', '999\nq\n', '3\n1\nq\n', '1\n' + 'yes'*100 + '\nq\n'):
            result = self.run_app(commands)
            self.assertEqual(result.returncode, 0)
            self.assertNotIn('MOCK-CUSTOM:', result.stdout)
            self.assertNotIn('MOCK-BOOT:', result.stdout)

    def test_custom_confirmation(self):
        result = self.run_app('1\nyes\nq\n')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.count('MOCK-CUSTOM:fake-custom'), 1)
        self.assertNotIn('MOCK-BOOT:', result.stdout)
        self.assertIn('Current build: test-build', result.stdout)

    def test_failure_stops_operation(self):
        result = subprocess.run([str(self.binary), str(self.package)], input='1\nyes\nq\n',
                                capture_output=True, text=True, timeout=5,
                                env={**os.environ, 'FAIL_CUSTOM': '1'})
        self.assertEqual(result.stdout.count('MOCK-CUSTOM:fake-custom'), 1)
        self.assertIn('Update failed (-4)', result.stdout)

    def test_bootloader_confirmation(self):
        result = self.run_app('2\nyes\nq\n')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.count('MOCK-BOOT:fake-boot'), 1)
        self.assertNotIn('MOCK-CUSTOM:', result.stdout)

    def selected(self, *options, commands='', env=None):
        return subprocess.run([str(self.cli), 'update', *map(str, options)], input=commands,
                              capture_output=True, text=True, timeout=5,
                              env={**os.environ, **(env or {})})

    def test_explicit_update_reuses_sessions_and_confirmation(self):
        for device, marker in [('fake-custom', 'MOCK-CUSTOM:fake-custom'), ('fake-boot', 'MOCK-BOOT:fake-boot')]:
            options = ['--device', device, '--product', 'U2C_WIRELESS', self.package]
            r = self.selected(*options, commands='yes\n')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count(marker), 1)
            self.assertNotIn('Select a device number', r.stdout)
            for answer in ('', '\n', 'no\n'):
                r = self.selected(*options, commands=answer)
                self.assertNotEqual(r.returncode, 0)
                self.assertNotIn('MOCK-CUSTOM:', r.stdout); self.assertNotIn('MOCK-BOOT:', r.stdout)
            r = self.selected(self.package, '--yes', '--product', 'U2C_WIRELESS', '--device', device)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count(marker), 1)
            self.assertNotIn('Type yes', r.stdout)

    def test_update_filters_and_direct_probe(self):
        r = self.selected(self.package, '--device', 'fake-custom', commands='yes\n')
        self.assertEqual(r.returncode, 0); self.assertEqual(r.stdout.count('MOCK-CUSTOM:fake-custom'), 1)
        r = self.selected('--product', 'U2C_WIRELESS', self.package, commands='q\n')
        self.assertEqual(r.returncode, 0); self.assertNotIn('fake-jieli', r.stdout)
        r = self.selected('--yes', '--device', 'fake-boot', '--product', 'U2C_WIRELESS', self.package, env={'NO_DEVICES':'1'})
        self.assertEqual(r.returncode, 0); self.assertEqual(r.stdout.count('MOCK-BOOT:fake-boot'), 1)
        for device, product in [('not-a-device', 'U2C_WIRELESS'), ('fake-custom', 'UNKNOWN'), ('fake-jieli', 'U2C_WIRELESS')]:
            r = self.selected('--device', device, '--product', product, '--yes', self.package)
            self.assertNotEqual(r.returncode, 0)
            self.assertNotIn('MOCK-CUSTOM:', r.stdout); self.assertNotIn('MOCK-BOOT:', r.stdout)

    def test_update_options_fail_before_io_and_direct_failures_stop(self):
        for args in [[], ['--device'], ['--product'], ['--unknown', self.package],
                     ['--yes', self.package], ['--yes', '--device', 'fake-custom', self.package],
                     ['--yes', '--product', 'U2C_WIRELESS', self.package],
                     ['--product','U2C_WIRELESS','--product','U2C_WIRELESS',self.package]]:
            r = self.selected(*args)
            self.assertEqual(r.returncode, 2, r.stdout+r.stderr)
            self.assertNotIn('Verified firmware', r.stdout)
        r = self.selected('--device','fake-custom','--product','U2C_WIRELESS','--yes',self.package, env={'FAIL_CUSTOM':'1'})
        self.assertEqual(r.returncode, 1); self.assertEqual(r.stdout.count('MOCK-CUSTOM:'), 1)
        self.assertIn('Update failed (-4)', r.stdout)

    def test_top_level_help_order_and_subcommand_lists(self):
        r = subprocess.run([str(self.cli), '--help'], capture_output=True, text=True, timeout=5)
        self.assertEqual(r.returncode, 0)
        lines = r.stdout.splitlines()
        entries = [line.split()[0] for line in lines if line.startswith('  ') and not line.startswith('    ')]
        self.assertEqual(entries[:5], ['scan','update','inspect','jieli','8bitdo'])
        self.assertIn('--device PATH', r.stdout); self.assertIn('--product PRODUCT', r.stdout)
        self.assertIn('enter-loader', r.stdout); self.assertNotIn('flash-package', r.stdout)
        for i, line in enumerate(lines):
            if line.startswith('  jieli ') or line.startswith('  8bitdo '):
                subcommands = []
                for detail in lines[i+1:]:
                    if not detail.startswith('    '): break
                    subcommands += [v.strip() for v in detail.strip().split(',') if v.strip()]
                self.assertIn('scan', subcommands); self.assertIn('restore', subcommands)
                self.assertNotIn('help', subcommands)

    def test_selected_jieli_product_and_unlisted_scsi_endpoint(self):
        from tools.test_jollymaam_jieli import JieliCliTests
        real_tool = Path(os.environ.get('JOLLYMAAM_TEST_APP', 'build/host/jollymaam')).resolve()
        if not real_tool.exists():
            self.skipTest('Build jollymaam for the Jieli package fixture')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);JieliCliTests().fixture(root)
            ini = root/'isd_config.ini'
            ini.write_text(ini.read_text().replace('PID=TEST;', 'PID=GS_C2_ADC_DEVICE;'))
            subprocess.run([str(real_tool), 'jieli', 'pack', '-c', str(ini), '-o', str(root/'install.bin'),
                            '--main', '0x1000:'+str(root/'main.bin'), '--main-out', str(root/'update.bin')],
                           check=True, capture_output=True)
            files = {'install.bin': (root/'install.bin').read_bytes(), 'update.bin': (root/'update.bin').read_bytes(), 'loader.bin': b'x'}
            def artifact(path):
                return dict(path=path, size=len(files[path]), sha256=hashlib.sha256(files[path]).hexdigest())
            target = dict(product='GS_C2_ADC_DEVICE', board='test-jieli', chip='BR23', preserve=[],
                          loader=artifact('loader.bin'), artifacts=[dict(role='install', **artifact('install.bin')),
                                                                  dict(role='update', **artifact('update.bin'))])
            package = root/'jieli.zip'
            with zipfile.ZipFile(package, 'w') as archive:
                archive.writestr('manifest.json', json.dumps(dict(format='jollymaam-firmware', version=1, targets=[target])))
                for name, data in files.items(): archive.writestr(name, data)
            for path in ('fake-jieli', 'fake-sg'):
                r = self.selected('--device', path, '--product', 'GS_C2_ADC_DEVICE', '--yes', package)
                self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
                self.assertEqual(r.stdout.count('MOCK-JIELI:'+path), 1)
                self.assertNotIn('UNEXPECTED-JIELI', r.stdout)
                self.assertNotIn('Target number', r.stdout)
                self.assertIn('Device remains in its loader', r.stdout)
