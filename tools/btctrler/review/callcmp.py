import re,subprocess,sys
from collections import Counter
T='/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin/clang'
for m in sys.argv[1:]:
    a=open(f'/work/cyclone2-firmware/build/controller-ir/sdk/{m}.c.o.ll').read()
    b=subprocess.run([T,'-target','pi32v2','-x','ir','-S','-emit-llvm',f'build/src/btctrler/{m}.o','-o','-'],capture_output=True,text=True).stdout
    def fns(t): return dict((x.group(1),x.group(0)) for x in re.finditer(r'^define [^\n]*@([\w.]+)\(.*?^}',t,re.M|re.S))
    A,B=fns(a),fns(b)
    def calls(t):
        c=Counter(re.findall(r'call [^@\n]*@([A-Za-z_][\w.]*)',t))
        return Counter({k:v for k,v in c.items() if not k.startswith(('llvm.','log_print','printf','puts','printf_buf'))})
    def mmio(t): return re.findall(r'(load|store) volatile [^\n]*inttoptr \(i32 (\d+)',t)
    diffs=0
    for n in sorted(set(A)&set(B)):
        ca,cb=calls(A[n]),calls(B[n]); ma,mb=mmio(A[n]),mmio(B[n])
        if ca!=cb or ma!=mb:
            diffs+=1
            print(f'  {m}.{n}: calls SDK-only {dict(ca-cb)} owned-only {dict(cb-ca)}' + ('' if ma==mb else f' MMIO {len(ma)} vs {len(mb)}'))
    print(f'{m}: {len(set(A)&set(B))} compared, {diffs} differ; SDK-only defs: {sorted(set(A)-set(B))}')
