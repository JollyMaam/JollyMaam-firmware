import re,subprocess,sys
T='/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin'
sdk=sys.argv[1]
for m in sys.argv[2:]:
    ir=open(f'{sdk}/{m}.c.o.ll').read()
    oll=subprocess.run([T+'/clang','-target','pi32v2','-x','ir','-S','-emit-llvm',f'build/src/btctrler/{m}.o','-o','-'],capture_output=True,text=True).stdout
    for line in ir.splitlines():
        g=re.match(r'^@([\w.]+) = (.*)$',line)
        if not g or g.group(1).startswith(('.str','__func__','str')) or g.group(2).startswith(('external','private')): continue
        name,rest=g.groups()
        sec=re.search(r'section "([^"]+)"',rest); al=re.search(r'align (\d+)',rest)
        kind='const' if ' constant ' in ' '+rest[:90] else 'var'
        o=re.search(r'^@'+re.escape(name)+r'(\.\d+)? = (.*)$',oll,re.M)
        if not o: print(f'{m:20} {name:30} SDK {kind} {sec and sec.group(1)} | not in owned object'); continue
        orest=o.group(2); osec=re.search(r'section "([^"]+)"',orest)
        okind='const' if ' constant ' in ' '+orest[:90] else 'var'
        ok=(sec and sec.group(1))==(osec and osec.group(1)) and kind==okind
        print(f'{m:20} {name:30} SDK {kind} {sec and sec.group(1)} al{al and al.group(1)} | own {okind} {osec and osec.group(1)} {"OK" if ok else "DIFF"}')
