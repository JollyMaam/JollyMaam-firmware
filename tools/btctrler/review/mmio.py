import re,subprocess,sys
T='/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin/clang'
sdk,obj=sys.argv[1],sys.argv[2]
a=open(sdk).read(); b=subprocess.run([T,'-target','pi32v2','-x','ir','-S','-emit-llvm',obj,'-o','-'],capture_output=True,text=True).stdout
def body(t,n):
    m=re.search(r'^define [^\n]*@'+n+r'\(.*?^}',t,re.M|re.S); return m.group(0) if m else ''
def seq(t):
    out=[]
    for l in t.splitlines():
        m=re.search(r'(load|store) volatile [^\n]*inttoptr \(i32 (\d+)',l)
        if m: out.append(f'{m.group(1)}:{int(m.group(2)):#x}')
        elif re.search(r'\bcall\b.*@(\w+)\(',l):
            c=re.search(r'@(\w+)\(',l).group(1)
            if not c.startswith(('llvm.','log_print','printf','printf_buf','puts')): out.append('call:'+c)
    return out
for n in sys.argv[3:]:
    sa,sb=seq(body(a,n)),seq(body(b,n))
    print(n,'SDK',len(sa),'owned',len(sb),'SAME' if sa==sb else 'DIFF')
    if sa!=sb:
        import difflib
        for d in difflib.unified_diff(sa,sb,lineterm='',n=1): print('   ',d)
