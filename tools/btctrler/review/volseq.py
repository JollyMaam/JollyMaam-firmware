"""Per function: ordered volatile accesses (MMIO address or struct field path) and calls, SDK vs owned."""
import re,subprocess,sys,difflib
T='/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin/clang'
member=sys.argv[1]; obj=sys.argv[2]; only=sys.argv[3:]
a=open(f'build/controller-ir/sdk/{member}.c.o.ll').read()
b=subprocess.run([T,'-target','pi32v2','-x','ir','-S','-emit-llvm',obj,'-o','-'],capture_output=True,text=True).stdout
def fns(t): return dict((m.group(1),m.group(0)) for m in re.finditer(r'^define [^\n]*@([\w.]+)\(.*?^}',t,re.M|re.S))
A,B=fns(a),fns(b)
def seq(body):
    defs={}
    for l in body.splitlines():
        m=re.match(r'\s*(%[\w.]+) = (.*)$',l)
        if m: defs[m.group(1)]=m.group(2)
    def resolve(p,depth=0):
        if depth>6: return '?'
        m=re.search(r'inttoptr \(i32 (\d+)',p)
        if m: return f'{int(m.group(1)):#x}'
        m=re.search(r'getelementptr inbounds \(?%struct\.(\w+)[^,]*, [^,]*%?[\w.@]+((?:, i32 [-\w%.]+)+)',p)
        if m: return m.group(1)+'['+','.join(x.split()[-1] if not x.split()[-1].startswith('%') else 'v' for x in m.group(2).split(', i32 ')[2:])+']'
        m=re.match(r'(%[\w.]+)$',p.strip())
        if m and m.group(1) in defs:
            d=defs[m.group(1)]
            if d.startswith('bitcast') or d.startswith('getelementptr'):
                inner=re.search(r'(%[\w.]+)(?:,| to)',d.split(' ',2)[-1] if d.startswith('bitcast') else d)
                if d.startswith('getelementptr'):
                    r=resolve(d,depth+1)
                    if r!='?': return r
                    base=re.findall(r'(%[\w.]+)',d)
                    return (resolve(base[1],depth+1) if len(base)>1 else '?')+'+idx'
                if inner: return resolve(inner.group(1),depth+1)
            if d.startswith('select') or d.startswith('phi'): return 'sel'
        m=re.search(r'getelementptr',p)
        if m: return resolve(re.sub(r'^.*?(getelementptr)',r'\1',p),depth+1)
        return '?'
    out=[]
    for l in body.splitlines():
        m=re.search(r'load volatile ([^,]+), [^,]+ ([^,]+)(?:, align|$)',l)
        if m: out.append('L'+m.group(1).strip()+':'+resolve(m.group(2))); continue
        m=re.search(r'store volatile ([^,]+?) [^,]+, [^,]+ ([^,]+)(?:, align|$)',l)
        if m: out.append('S'+m.group(1).strip()+':'+resolve(m.group(2))); continue
        m=re.search(r'call [^@\n]*@([A-Za-z_][\w.]*)',l)
        if m and not m.group(1).startswith(('llvm.','log_print','printf','puts','cpu_assert_debug','cpu_reset','p33_soft_reset','btctrler_assert_failed')):
            out.append('C:'+m.group(1))
    return out
diff=0
for n in sorted(set(A)&set(B)):
    if only and n not in only: continue
    sa,sb=seq(A[n]),seq(B[n])
    if sa!=sb:
        diff+=1
        print(f'== {n}: SDK {len(sa)} owned {len(sb)}')
        for d in list(difflib.unified_diff(sa,sb,lineterm='',n=0))[2:14]: print('   ',d)
print('compared',len(set(A)&set(B)) if not only else len(only),'differ',diff,'| SDK-only defs:',sorted(set(A)-set(B)),'| owned-only:',sorted(set(B)-set(A)))
