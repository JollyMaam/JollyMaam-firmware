#!/usr/bin/env python3
"""Compare BD19 DUT prestart arithmetic with isolated original SDK LLVM.

Only arithmetic, masked register inputs and anchor write results are compared;
this does not establish MMIO, interrupt, allocator or radio equivalence.
Requires decoded controller IR, a host clang accepting it, and a host C compiler.
"""
from pathlib import Path
import re,subprocess,ctypes as C,random,argparse,shutil,tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang',default=shutil.which('clang'))
parser.add_argument('--cc',default='gcc')
args=parser.parse_args()
if not args.clang:parser.error('pass --clang PATH to a host clang')
workspace=tempfile.TemporaryDirectory(prefix='btctrler-dut-prestart-')
out=Path(workspace.name)
root=Path(__file__).resolve().parents[2]
raw=(root/'build/controller-ir/bd19/01--direct_test_dut.c.o.ll').read_text()
f=re.search(r'^define[^\n]*@prestart_cnt_adjust\([^\n]*\n.*?^}',raw,re.M|re.S)[0]
f='\n'.join(l for l in f.splitlines() if '@llvm.dbg.' not in l)
f=re.sub(r',? ![\w.]+ !\d+','',f);f=re.sub(r'#\d+','',f);f=f.replace('define internal fastcc','define').replace(' fastcc ',' ').replace('@prestart_cnt_adjust','@oracle_prestart')
f=f.replace('inttoptr (i32 1867788 to i32*)','@regA').replace('inttoptr (i32 1867796 to i32*)','@regB')
f=f.replace('@__get_interval','@get_interval').replace('@__get_ble_anchor_con','@get_anchor').replace('@__set_ble_anchor_con','@set_anchor')
ir='%struct.ble_hw = type opaque\n@regA = external global i32\n@regB = external global i32\ndeclare i32 @get_interval()\ndeclare i32 @get_anchor(i8)\ndeclare void @set_anchor(i8,i32)\n'+f+'\n'
(out/'prestart-oracle.ll').write_text(ir)
clang=args.clang
subprocess.run([clang,'-Wno-override-module','-fPIC','-x','ir','-c',str(out/'prestart-oracle.ll'),'-o',str(out/'prestart-oracle.o')],check=True)
mock='''#include <stdint.h>
uint32_t regA,regB,interval,anchor,written;int writes;
uint32_t get_interval(void){return interval;}
uint32_t get_anchor(uint8_t index){return anchor;}
void set_anchor(uint8_t index,uint32_t value){written=value;writes++;}
'''
(out/'prestart-mocks.c').write_text(mock)
subprocess.run([args.cc,'-shared','-fPIC',str(out/'prestart-oracle.o'),str(out/'prestart-mocks.c'),'-o',str(out/'prestart-oracle.so')],check=True)
s=(root/'src/btctrler/bd19/direct_test_dut.c').read_text()
f=re.search(r'^static void prestart_cnt_adjust\([^;]*?\n\{.*?^}',s,re.M|re.S)[0]
f=f.replace('static void prestart_cnt_adjust','void own_prestart').replace('__get_interval','get_interval').replace('__get_ble_anchor_con','get_anchor').replace('__set_ble_anchor_con','set_anchor')
source='#include <stdint.h>\ntypedef uint32_t u32;typedef int32_t s32;typedef uint16_t u16;typedef int16_t s16;struct ble_hw;\nextern uint32_t regA,regB;uint32_t get_interval(void);uint32_t get_anchor(uint8_t);void set_anchor(uint8_t,uint32_t);\n#define DUT_REG32(a) ((a)==1867788?regA:regB)\n'+f+'\n'
(out/'prestart-owned.c').write_text(source)
subprocess.run([args.cc,'-shared','-fPIC','-O2',str(out/'prestart-owned.c'),str(out/'prestart-mocks.c'),'-o',str(out/'prestart-owned.so')],check=True)
a=C.CDLL(str(out/'prestart-oracle.so'));b=C.CDLL(str(out/'prestart-owned.so'))
for lib,name in [(a,'oracle_prestart'),(b,'own_prestart')]:getattr(lib,name).argtypes=[C.c_void_p]
rng=random.Random(192619);count=0
vectors=[(interval,anchor,regA,regB) for interval in [0,1,2,0xffffffff] for anchor in [0,1,2047,2048,4095,32767,32768,65535,0x80000200,0xffffffff] for regA in [0,600,625,1023,0xffffffff] for regB in [0,15,34,255,0xffffffff]]
vectors += [(rng.choice([0,1,2,rng.getrandbits(32)]),rng.getrandbits(32),rng.getrandbits(32),rng.getrandbits(32)) for _ in range(100000)]
for vals in vectors:
 outputs=[]
 for lib,name in [(a,'oracle_prestart'),(b,'own_prestart')]:
  for key,v in zip(['interval','anchor','regA','regB'],vals):C.c_uint32.in_dll(lib,key).value=v
  C.c_int.in_dll(lib,'writes').value=0;C.c_uint32.in_dll(lib,'written').value=0x1234
  getattr(lib,name)(None)
  outputs.append((C.c_int.in_dll(lib,'writes').value,C.c_uint32.in_dll(lib,'written').value))
 assert outputs[0]==outputs[1],(vals,outputs)
 count+=1
print('prestart raw SDK LLVM vs recovered C:',count,'cases passed')
