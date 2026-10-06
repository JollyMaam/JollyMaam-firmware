import re,glob,subprocess,sys
sys.path.insert(0,'tools'); import btctrler.recover as r
owned=set()
for k,v in vars(r).items():
    if k.endswith('_OWNED') and isinstance(v,dict): owned|=set(v)
def funcs(path):
    t=open(path).read(); return re.findall(r'^define [^@]*@"?([\w.$]+)"?\(',t,re.M)
ret={p:open(p).read() for p in glob.glob('build/controller-ir/retained/*.ll')}
src=subprocess.run('cat src/*.c src/btctrler/*.c',shell=True,capture_output=True,text=True).stdout
HELP={'cpu_reset','cpu_in_irq','cpu_irq_disabled','list_add','list_del','list_add_tail','INIT_LIST_HEAD','list_empty','__list_add','__list_del'}
for m in sys.argv[1:]:
    left=[n for n in funcs(f'build/controller-ir/sdk/{m}.c.o.ll') if n not in owned]
    bad=[]
    for n in left:
        if n in HELP: continue
        hits=[p.split('/')[-1] for p,t in ret.items() if re.search(r'@'+re.escape(n)+r'\b',t)]
        if hits or re.search(r'\b'+re.escape(n)+r'\s*\(',src): bad.append((n,hits[:3]))
    print(f'{m:20} {len(left)} leftovers; referenced elsewhere: {bad}')
