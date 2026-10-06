"""For every owned function: wide (i16/i32/i64) align-1 loads/stores in the SDK body vs the owned object."""
import re,glob,subprocess,sys
from collections import Counter
sys.path.insert(0,'tools'); import btctrler.recover as r
owned=set()
for k,v in vars(r).items():
    if k.endswith('_OWNED') and isinstance(v,dict): owned|=set(v)
T='/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin/clang'
def fns(t): return dict((m.group(1),m.group(0)) for m in re.finditer(r'^define [^\n]*@([\w.]+)\(.*?^}',t,re.M|re.S))
sdk={}
for p in glob.glob('build/controller-ir/sdk/*.ll'):
    for n,b in fns(open(p).read()).items():
        if n in owned: sdk.setdefault(n,(p,b))
own={}
for o in glob.glob('build/src/btctrler/*.o'):
    t=subprocess.run([T,'-target','pi32v2','-x','ir','-S','-emit-llvm',o,'-o','-'],capture_output=True,text=True).stdout
    for n,b in fns(t).items(): own.setdefault(n,(o,b))
W=re.compile(r'(load|store) (?:volatile )?(i16|i32|i64)[ ,][^\n]*align 1\b')
def wide(b):
    c=Counter()
    for l in b.splitlines():
        m=W.search(l)
        if m: c[m.group(1)+' '+m.group(2)]+=1
    return c
flag=0
for n,(p,b) in sorted(sdk.items()):
    ws=wide(b)
    if not ws: continue
    if n not in own:
        print(f'{n}: SDK {dict(ws)} — owned body not found (inlined?)'); continue
    wo=wide(own[n][1])
    # owned may express via memcpy; count memcpy sizes too
    mc=Counter(re.findall(r'llvm\.mem(?:cpy|move)[^\n]*i32 (\d+), i32',own[n][1]))+Counter(re.findall(r'call [^@\n]*@mem(?:cpy|move|set)\([^\n]*i32 (\d+)\)',own[n][1]))+Counter('call:'+x for x in re.findall(r'call [^@\n]*@(\w*(?:cpy|copy|reverse|store|swap|flip|endian)\w*)\(',own[n][1]))
    missing={k:v-wo[k] for k,v in ws.items() if wo[k]<v}
    if missing:
        flag+=1
        print(f'{n} [{p.split("/")[-1]} -> {own[n][0].split("/")[-1]}]: SDK {dict(ws)} owned {dict(wo)} memcpy {dict(mc)}')
print('functions flagged:',flag)
