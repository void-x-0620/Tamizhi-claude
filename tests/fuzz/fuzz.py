import random, subprocess, sys, glob, os, re, tempfile
ROOT=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TMP=tempfile.mkdtemp()
random.seed(int(sys.argv[1]) if len(sys.argv)>1 else 1)
N=int(sys.argv[2]) if len(sys.argv)>2 else 1500
BIN=os.environ.get('BIN') or os.path.join(ROOT,'tamizhi-asan')
seeds=[open(f,errors='replace').read() for f in glob.glob(ROOT+'/tests/**/*.tz',recursive=True)+glob.glob(ROOT+'/examples/**/*.tz',recursive=True)]
tok=re.compile(r'\s+|[A-Za-z_][A-Za-z0-9_]*|\d+\.?\d*|"[^"\n]*"|\'[^\'\n]*\'|.',re.S)
frag=['{','}','(',')',';',':','<','>','[',']','..','.','@','^','*','&','=','==','+=','"unterminated','\'','0x','1e','/*','#','seyal','vagupu','moolam','muyarchi','pidi','ulle','List<','$"{x}"','Fn<','Map<','Set<','puthu','thandhai','ithu','nilai','illai','kudu','erivi','serku "x.tzh";','int','string','99999999999999999999','\x00','é','தமிழ்']
def mutate(s):
    t=tok.findall(s)
    for _ in range(random.randint(2,8)):
        if not t: break
        i=random.randrange(len(t)); k=random.random()
        if k<.25: del t[i]
        elif k<.45: t.insert(i,random.choice(frag))
        elif k<.6: t[i]=random.choice(frag)
        elif k<.75: j=random.randrange(len(t)); t[i],t[j]=t[j],t[i]
        elif k<.9: t.insert(i,t[i])
        else: t=t[:i]
    return ''.join(t)
crashes={}
for n in range(N):
    src=mutate(random.choice(seeds))
    p=TMP+'/in.tz'; open(p,'w',errors='replace').write(src)
    try:
        r=subprocess.run([BIN,p,'-S','--no-gc','--color=never'],capture_output=True,timeout=10,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    except subprocess.TimeoutExpired:
        key='TIMEOUT'; err=b''; rc=-999
    else:
        rc=r.returncode; err=r.stderr
        bad = rc<0 or b'AddressSanitizer' in err or b'runtime error' in err[:0]
        if not bad: continue
        m=re.search(rb'#\d+ 0x[0-9a-f]+ in (\S+) (\S+)',err)
        key=(m.group(1)+b' '+m.group(2)).decode() if m else f'rc={rc}'
    if key not in crashes:
        crashes[key]=src; open(f'{TMP}/crash_{len(crashes)}.tz','w',errors='replace').write(src)
        print('NEW',key, rc, err.split(b'\n')[1][:100] if err else b'')
print('fuzz: %d inputs, %d distinct crashes/hangs'%(N,len(crashes)), '(kept in '+TMP+')' if crashes else '')
sys.exit(1 if crashes else 0)
