import os
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
int jm_hid_8bitdo_model(const char *p,uint16_t *m) { (void)p; *m=0x301b; return 1; }
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
const struct jm_session_io *jm_platform_io(void) { static const struct jm_session_io io={0};return &io; }
int jm_session_run(const struct jm_firmware_target *t,enum jm_session_kind kind,const uint8_t uid[16],
                    const struct jm_session_io *io,void *context,jm_session_progress cb,void *u) {
 (void)io;struct jm_platform_connection *p=context;
 if(kind==JM_SESSION_CUSTOM) return jm_hid_custom_update(p->path,uid,t->update.data,t->update.size,cb,u);
 if(kind==JM_SESSION_8BITDO) { printf("MOCK-BOOT:%s\\n",p->path);return 0; }
 return jm_jieli_upgrade(p->path,t->loader.data,t->loader.size,t->install.data,t->install.size);
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
int main(int argc,char **argv) { return argc==2 ? jollymaam_update_app(argv[1]) : 2; }
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
        result = subprocess.run([str(self.cli), 'stock-loader', '--yes', 'fake-custom'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn('MOCK-STOCK-LOADER', result.stdout)
        result = subprocess.run([str(self.cli), 'stock-loader', 'fake-custom'], capture_output=True, text=True, timeout=5)
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
