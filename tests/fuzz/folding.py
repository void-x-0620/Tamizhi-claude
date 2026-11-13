import random, subprocess, sys, os, tempfile
ROOT=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TMP=tempfile.mkdtemp()
random.seed(int(sys.argv[1]))
def tdiv(a,b): q=abs(a)//abs(b); return q if (a<0)==(b<0) else -q
def tmod(a,b): return a-tdiv(a,b)*b
ops=['+','-','*','/','%','&','|','^','<<','>>','<','<=','>','>=','==','!=','&&','||']
def gen(d, vars_):
    if d==0 or random.random()<.25:
        v=random.randint(-20,40)
        return (str(v) if v>=0 else f'(-{-v})', v, vars_.append(v) or len(vars_)-1)
    k=random.random()
    if k<.12:
        s,v,_=gen(d-1,vars_); return (f'(-{s})',-v,None) if False else (f'(-{s})',-v,'x')
    if k<.18:
        s,v,_=gen(d-1,vars_); return (f'(!{s})',int(v==0),'x')
    if k<.24:
        s,v,_=gen(d-1,vars_); return (f'(~{s})',~v,'x')
    op=random.choice(ops)
    a,av,_=gen(d-1,vars_); b,bv,_=gen(d-1,vars_)
    if op in '/%' and bv==0: return a,av,'x'
    if op in ('<<','>>'):
        if not (0<=bv<=20) or av<0: return a,av,'x'
    if op=='<<' and abs(av)>>40: return a,av,'x'
    if op=='*' and abs(av*bv)>2**40: return a,av,'x'
    r={'+':lambda:av+bv,'-':lambda:av-bv,'*':lambda:av*bv,'/':lambda:tdiv(av,bv),'%':lambda:tmod(av,bv),
       '&':lambda:av&bv,'|':lambda:av|bv,'^':lambda:av^bv,'<<':lambda:av<<bv,'>>':lambda:av>>bv,
       '<':lambda:int(av<bv),'<=':lambda:int(av<=bv),'>':lambda:int(av>bv),'>=':lambda:int(av>=bv),
       '==':lambda:int(av==bv),'!=':lambda:int(av!=bv),'&&':lambda:int(bool(av) and bool(bv)),'||':lambda:int(bool(av) or bool(bv))}[op]()
    return (f'({a} {op} {b})', r, 'x')
lines=[];exp=[]
for i in range(150):
    vars_=[]
    s,v,_=gen(4,vars_)
    # runtime form: replace literals by variables via a different strategy: evaluate through a function with parameter-free locals
    lines.append(f'    pathi("%d\\n", {s});'); exp.append(v)
    # variable form: same text but with "edu k = 0;" trick: add (k) zero to every literal
    import re
    s2=re.sub(r'(?<![\w])(\d+)(?![\w])',lambda m:f'(k+{m.group(1)})',s)
    lines.append(f'    pathi("%d\\n", {s2});'); exp.append(v)
src='seyal main(): int {\n    edu k = 0;\n'+'\n'.join(lines)+'\n    kudu 0;\n}\n'
open(TMP+'/d.tz','w').write(src)
r=subprocess.run([os.environ.get('BIN') or os.path.join(ROOT,'tamizhi'),TMP+'/d.tz','--no-gc','--no-warn','-o',TMP+'/d'],capture_output=True,text=True)
if r.returncode: print(r.stderr[:600]); sys.exit(1)
out=subprocess.run([TMP+'/d'],capture_output=True,text=True).stdout.split()
bad=[(i,lines[i],exp[i],out[i]) for i in range(len(exp)) if int(out[i])!=exp[i]]
print('constant folding vs runtime: %d checked, %d mismatches'%(len(exp),len(bad)))
if bad: sys.exit(1)
for b in bad[:5]: print(b)
