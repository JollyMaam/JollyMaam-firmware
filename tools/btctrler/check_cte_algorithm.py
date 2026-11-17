#!/usr/bin/env python3
"""Compare recovered CTE arithmetic with original SDK LLVM on a host.

Requires a host clang accepting the pinned LLVM IR and gcc. The adapted SDK
oracle removes debug metadata and target attributes, renames definitions, and
exports private helpers. It is additional evidence, not target/IRQ equivalence.
The primarydata record has no pointers; its field offsets match on the host.
"""
import argparse, ctypes as C, random, struct, math, re, subprocess, tempfile, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang',default=shutil.which('clang'),help='host LLVM clang executable')
parser.add_argument('--cc',default='gcc',help='host C compiler')
args=parser.parse_args()
if not args.clang:parser.error('pass --clang PATH to a host clang')
workspace=tempfile.TemporaryDirectory(prefix='btctrler-cte-')
out=Path(workspace.name)
original=ROOT/'build/controller-ir/bd19/01--cte_data_algorithm.c.o.ll'
sdk_ir=original.read_text()
sdk_ir='\n'.join(l for l in sdk_ir.splitlines() if not l.startswith('!') and '@llvm.dbg.' not in l)
sdk_ir=re.sub(r',? ![\w.]+ !\d+','',sdk_ir)
sdk_ir=re.sub(r'^target datalayout = .*\n','',sdk_ir,flags=re.M)
names=re.findall(r'^(?:define[^\n]*@|@)([\w.]+)',sdk_ir,re.M)
for n in sorted(set(names),key=len,reverse=True):sdk_ir=re.sub(r'@'+re.escape(n)+r'(?![\w.])','@sdk_'+n,sdk_ir)
sdk_ir=sdk_ir.replace('define internal ','define ').replace(' fastcc ',' ')
sdk_ir=sdk_ir.replace('@sdk_primarydata_instance = internal ', '@sdk_primarydata_instance = ')
sdk_ir=sdk_ir.replace('"allow-nullptr-deref"','').replace('"target-features"="+no-constpool"','')
sdk_ir=sdk_ir.replace('@printf(','@cte_test_printf(').replace('@puts(','@cte_test_puts(')
(out/'sdk.ll').write_text(sdk_ir+'\n')
subprocess.run([args.clang,'-fPIC','-Wno-override-module','-target','x86_64-unknown-linux-gnu','-x','ir','-c',str(out/'sdk.ll'),'-o',str(out/'sdk.o')],check=True)
(out/"silent.c").write_text('static double values[32]; static int count;\nvoid test_log_reset(void){count=0;}\nint test_log_count(void){return count;}\ndouble test_log_value(int i){return values[i];}\nvoid sdk_put_float(double f){if(count<32)values[count++]=f;}\nvoid put_float(double f){if(count<32)values[count++]=f;}\nint cte_test_printf(const char *f,...){return 0;}\nint cte_test_puts(const char *s){return 0;}\nvoid log_print(int l,void *a,const char *f,...){}\n')
source=(ROOT/'src/platform/jieli/btctrler/bd19/cte_data_algorithm.c').read_text()
# Export helpers only in the temporary comparison translation unit.
source=source[:source.index('/* Host regression fixtures. */')]
for n in ['angle_est_fun','data_pre_processing','mac_data_load']:
 source=source.replace('static float '+n,'float '+n).replace('static void '+n,'void '+n)
source=source.replace('static struct primarydata *primarydata_instance;', 'struct primarydata *primarydata_instance;')
source='#define printf cte_test_printf\n#define puts cte_test_puts\n'+source
(out/'owned.c').write_text(source)
for name,inputs in [('sdk',[out/'sdk.o']),('owned',[out/'owned.c'])]:
 subprocess.run([args.cc,'-shared','-fPIC','-O2','-ffp-contract=off','-DUNITTEST','-DCONFIG_CPU_BD19','-iquote',str(ROOT/'src'),*[str(p) for p in inputs],str(out/'silent.c'),'-o',str(out/(name+'.so'))],check=True)
class Context(C.Structure):
 _pack_=4
 _fields_=[('reference_data_r',C.c_int16*8),('reference_data_i',C.c_int16*8),('sam_slot_data_r',C.c_int16*76),('sam_slot_data_i',C.c_int16*76),('sam_slot_num',C.c_uint8),('freq_offset',C.c_int16),('RSSI',C.c_int8),('AGC_index',C.c_int8),('antenna_max_amp',C.c_int32),('antenna_num',C.c_uint8),('antenna_num_row',C.c_uint8),('antenna_num_col',C.c_uint8),('reference_data_abs_sum',C.c_double),('sampleslot_data_abs_sum',C.c_double),('data_len',C.c_uint16),('wave_len',C.c_float),('antenna_dista',C.c_float),('amp_jitter',C.c_float),('pha_jitter',C.c_float)]
assert C.sizeof(Context)==388
sdk=C.CDLL(str(out/'sdk.so'));own=C.CDLL(str(out/'owned.so'));P=C.POINTER(Context);i=C.c_int;u8=C.c_uint8;s16=C.c_int16;f=C.c_float
spec={'atan_fun':(i,[i,i]),'sin_func':(s16,[C.POINTER(s16)]),'complex_multi':(None,[i,i,i,i,C.POINTER(i),C.POINTER(i)]),'frequency_offset':(i,[u8,f,s16,u8,P]),'cfo_est_fun':(f,[u8,u8,u8,P]),'distance_est':(f,[P,f,f]),'IQ_data_assessment':(i,[P]),'data_pre_processing':(None,[C.c_void_p,P]),'mac_data_load':(None,[P,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]),'angle_est_fun':(f,[P]),'cte_data_algorithm_module_init':(i,[]),'cte_data_algorithm_module_close':(None,[]),'cte_data_conversion':(i,[C.c_void_p,u8,u8,C.c_void_p,C.c_void_p]),'cte_data_angle_calc':(i,[C.c_void_p,u8,u8,C.c_void_p,C.c_void_p])}
for n,(rest,args) in spec.items():
 for lib,prefix in [(sdk,'sdk_'),(own,'')]:
  fun=getattr(lib,prefix+n);fun.restype=rest;fun.argtypes=args
for lib in [sdk,own]:lib.test_log_value.argtypes=[i];lib.test_log_value.restype=C.c_double
rng=random.Random(192619);count=0

def bits(v):return struct.pack('f',v)
def equal(n,args1,args2=None):
 global count
 sdk.test_log_reset();own.test_log_reset()
 a=getattr(sdk,'sdk_'+n)(*args1);b=getattr(own,n)(*(args1 if args2 is None else args2));count+=1
 assert sdk.test_log_count()==own.test_log_count(),('log count',n,sdk.test_log_count(),own.test_log_count())
 for j in range(sdk.test_log_count()):
  x=sdk.test_log_value(j);y=own.test_log_value(j)
  assert x==y or (math.isnan(x) and math.isnan(y)),('log value',n,j,x,y)
 assert (bits(a)==bits(b) if isinstance(a,float) else a==b),(n,args1,a,b)
 return a

def ctx():
 x=Context();x.antenna_num_row=rng.randint(2,4);x.antenna_num_col=rng.randint(1,4);x.antenna_num=x.antenna_num_row*x.antenna_num_col;x.sam_slot_num=rng.randint(x.antenna_num+1,68);x.wave_len=12.5;x.antenna_dista=4.35;x.data_len=4*(x.sam_slot_num+7)+26
 for name in ['reference_data_r','reference_data_i','sam_slot_data_r','sam_slot_data_i']:
  a=getattr(x,name)
  for j in range(len(a)):a[j]=rng.randint(-2000,2000)
 x.reference_data_abs_sum=rng.uniform(10000,1000000);x.sampleslot_data_abs_sum=rng.uniform(100,100000000);x.RSSI=rng.randint(-80,-10);x.AGC_index=rng.randint(-10,10)
 return x

def clone(x):return Context.from_buffer_copy(bytes(x))
for j in range(-32768,32768):
 a=s16(j);b=s16(j);equal('sin_func',[C.byref(a)],[C.byref(b)]);assert a.value==b.value
for j in range(50000):equal('atan_fun',[rng.randint(-10000000,10000000),rng.randint(-10000000,10000000)])
for x in [-1,0,1,256,-256]:
 for y in [-1,0,1,256,-256]:equal('atan_fun',[x,y])
for j in range(2000):
 x=ctx();y=clone(x)
 equal('cfo_est_fun',[x.sam_slot_num,x.antenna_num,0,C.byref(x)])
 equal('cfo_est_fun',[8,1,1,C.byref(x)])
 equal('angle_est_fun',[C.byref(x)])
 phase=f(rng.uniform(-10,10));start=s16(rng.randint(-500,500));ref=rng.randint(0,1)
 equal('frequency_offset',[x.sam_slot_num,phase,start,ref,C.byref(x)],[y.sam_slot_num,phase,start,ref,C.byref(y)]);assert bytes(x)==bytes(y),('frequency context',j)
 # Diagnostics read amp_jitter/pha_jitter locally, while writing neither.
 equal('IQ_data_assessment',[C.byref(x)])
 equal('distance_est',[C.byref(x),f(-50),f(2.5)])
 for ratio in ([0,20,22,200000,float('nan'),float('inf')] if j<5 else []):
  x.sampleslot_data_abs_sum=ratio;x.reference_data_abs_sum=2000
  equal('distance_est',[C.byref(x),f(-50),f(2.5)])
 if j<500:
  x=ctx();y=clone(x);raw=(C.c_uint8*400)(*(rng.randrange(256) for _ in range(400)));x.data_len=y.data_len=min(x.data_len,354)
  equal('data_pre_processing',[raw,C.byref(x)],[raw,C.byref(y)]);assert bytes(x)==bytes(y),('preprocess context',j)
  # preprocessor may generate s16 extremes; pipeline math permits pair wrap.
  a=(C.c_uint8*100)();b=(C.c_uint8*100)();aa=(C.c_uint8*100)();bb=(C.c_uint8*100)();r=u8();g=u8();rr=u8();gg=u8()
  equal('mac_data_load',[C.byref(x),a,b,C.byref(r),C.byref(g)],[C.byref(y),aa,bb,C.byref(rr),C.byref(gg)])
  assert bytes(x)==bytes(y) and bytes(a)==bytes(aa) and bytes(b)==bytes(bb) and r.value==rr.value and g.value==gg.value,('mac output',j)
# Full public pipeline against the embedded SDK sample, including diagnostic calls.
raw=(C.c_uint8*354).in_dll(sdk,'sdk_cte_data')
for samples in [32,40,64,80,81]:
 equal('cte_data_algorithm_module_init',[])
 sp=C.c_void_p.in_dll(sdk,'sdk_primarydata_instance').value
 op=C.c_void_p.in_dll(own,'primarydata_instance').value
 C.memset(sp,0,388);C.memset(op,0,388)
 a=(C.c_uint8*100)();b=(C.c_uint8*100)();aa=(C.c_uint8*100)();bb=(C.c_uint8*100)()
 for fn in ['cte_data_conversion','cte_data_angle_calc']:
  equal(fn,[raw,samples,16,a,b],[raw,samples,16,aa,bb])
  assert C.string_at(sp,388)==C.string_at(op,388),(fn,'context',samples)
  assert bytes(a)==bytes(aa) and bytes(b)==bytes(bb),(fn,'output',samples)
 equal('cte_data_algorithm_module_close',[])
print('SDK LLVM host differential calls:' ,count,'all exact; sinusoid domain exhaustive')
