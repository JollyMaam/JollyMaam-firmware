"""Resolve cherry-pick conflicts in the btctrler tables by keeping both sides."""
import re,sys
def merge_tuple_line(a,b):
    # union of quoted names preserving order
    names=[]
    for n in re.findall(r"'[^']+'",a)+re.findall(r"'[^']+'",b):
        if n not in names: names.append(n)
    return names
def resolve(path):
    s=open(path).read()
    def fix(m):
        a,b=m.group(1),m.group(2)
        if 'DROPPED' in a or 'DROPPED' in b:
            names=merge_tuple_line(a,b)
            # rebuild DROPPED block lines (both sides contain first line only usually)
            close = ')' if a.rstrip().endswith(')') or b.rstrip().endswith(')') else ','
            return 'DROPPED = (' + ', '.join(names) + close + '\n'
        if 'from .recover import' in a:
            ia=re.search(r'from \.recover import (.*)',a).group(1).split(', ')
            ib=re.search(r'from \.recover import (.*)',b).group(1).split(', ')
            imp=ia+[x for x in ib if x not in ia]
            oa=re.findall(r'tuple\((\w+)\)',a); ob=re.findall(r'tuple\((\w+)\)',b)
            own=oa+[x for x in ob if x not in oa]
            tail=re.search(r'OWNED = (?:tuple\(\w+\) \+ )+(.*)',a).group(1)
            return 'from .recover import '+', '.join(imp)+'\nOWNED = '+' + '.join(f'tuple({x})' for x in own)+' + '+tail+'\n'
        return a + ('' if a.endswith('\n\n') else '\n') + b
    s2=re.sub(r'<<<<<<< [^\n]*\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n',fix,s,flags=re.S)
    open(path,'w').write(s2)
    return '<<<<<<<' not in s2
for p in sys.argv[1:]: print(p, resolve(p))
