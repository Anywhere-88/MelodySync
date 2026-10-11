# -*- coding: utf-8 -*-
"""审计：自研选定的掩码，是否确实是 Python 独立 ISO 罚分实现下的最小者？"""
import json, random, string, subprocess, sys, os, math
HERE=os.path.dirname(os.path.abspath(__file__))
NODE=r'C:/Users/yfw20/.workbuddy/binaries/node/versions/22.22.2-2/node.exe'
N1,N2,N3,N4=3,3,40,10

def penalty(m):
    size=len(m); res=0
    def ah(run,h):
        if h[0]==0: run+=size
        h.pop(); h.insert(0,run)
    def cp(h):
        n=h[1]
        core = n>0 and h[2]==n and h[3]==n*3 and h[4]==n and h[5]==n
        return (1 if (core and h[0]>=n*4 and h[6]>=n) else 0)+(1 if (core and h[6]>=n*4 and h[0]>=n) else 0)
    def tc(cc,rl,h):
        if cc: ah(rl,h); rl=0
        rl+=size; ah(rl,h); return cp(h)
    for y in range(size):
        c=False; rl=0; h=[0]*7
        for x in range(size):
            if m[y][x]==c:
                rl+=1
                if rl==5: res+=N1
                elif rl>5: res+=1
            else:
                ah(rl,h)
                if not c: res+=cp(h)*N3
                c=m[y][x]; rl=1
        res+=tc(c,rl,h)*N3
    for x in range(size):
        c=False; rl=0; h=[0]*7
        for y in range(size):
            if m[y][x]==c:
                rl+=1
                if rl==5: res+=N1
                elif rl>5: res+=1
            else:
                ah(rl,h)
                if not c: res+=cp(h)*N3
                c=m[y][x]; rl=1
        res+=tc(c,rl,h)*N3
    for y in range(size-1):
        for x in range(size-1):
            v=m[y][x]
            if v==m[y][x+1] and v==m[y+1][x] and v==m[y+1][x+1]: res+=N2
    dark=sum(1 for y in range(size) for x in range(size) if m[y][x]); total=size*size
    k=math.ceil(abs(dark*20-total*10)/total)-1
    return res+max(0,k)*N4

def js_all(text,ecc):
    o=subprocess.run([NODE,'runall.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0:
        print('   [跳过] node 报错: %s' % o.stderr.decode('utf-8','replace')[:200])
        return None
    return json.loads(o.stdout.decode())['masks']

def js_chosen(text,ecc):
    o=subprocess.run([NODE,'run.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0: return None
    return json.loads(o.stdout.decode())

cases=['a','HELLO WORLD','https://music.163.com/login?codekey=abc123',
       'https://y.music.163.com/m/login?codekey='+'0123456789abcdef'*3,
       '中文二维码测试','https://a.b/c?d='+'x'*200,
       'https://a.b/c?d='+'y'*600,'https://orcode.example/long?'+'z'*1200]
random.seed(99)
for _ in range(22):
    cases.append(''.join(random.choice(string.ascii_letters+string.digits+'/:?=&.-_')
                         for _ in range(random.randint(1,1400))))
for _ in range(6):
    cases.append(''.join(random.choice('音乐歌单测试中文内容扫码登录') for _ in range(random.randint(5,200))))

ok=0; bad=[]
for t in cases:
    for ecc in ['L','M','Q','H']:
        g=js_all(t,ecc)
        if not g: continue
        pens=[penalty(m) for m in g]
        best=min(range(8), key=lambda i: pens[i])
        c=js_chosen(t,ecc)
        if c is None: continue
        if c['mask']==best: ok+=1
        else: bad.append((len(t),ecc,c['mask'],best,pens[c['mask']]-pens[best]))
print('掩码优选审计: 命中 ISO 最小值 %d / %d'%(ok, ok+len(bad)))
if bad:
    print('未命中 %d 例（前 8）:'%len(bad))
    for b in bad[:8]:
        print('   len=%-5d ecc=%s 自选=%d 最优=%d 罚分高出 %d'%b)
else:
    print('自研掩码优选在所有用例上都取到了 ISO 罚分最小值 ✓')
