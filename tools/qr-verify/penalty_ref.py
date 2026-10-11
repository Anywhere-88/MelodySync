# -*- coding: utf-8 -*-
"""Python 独立实现 ISO/IEC 18004 第 8.8.2 节罚分，与 JS 版本交叉验证。"""
import json, subprocess, os, sys
HERE=os.path.dirname(os.path.abspath(__file__))
NODE=r'C:/Users/yfw20/.workbuddy/binaries/node/versions/22.22.2-2/node.exe'
N1,N2,N3,N4=3,3,40,10

def penalty(m):
    size=len(m)
    res=0
    def add_hist(run,h):
        if h[0]==0: run+=size
        h.pop(); h.insert(0,run)
    def count_pat(h):
        n=h[1]
        core = n>0 and h[2]==n and h[3]==n*3 and h[4]==n and h[5]==n
        a = 1 if (core and h[0]>=n*4 and h[6]>=n) else 0
        b = 1 if (core and h[6]>=n*4 and h[0]>=n) else 0
        return a+b
    def term(cur_color,cur_len,h):
        if cur_color: add_hist(cur_len,h); cur_len=0
        cur_len+=size; add_hist(cur_len,h); return count_pat(h)
    # 行
    for y in range(size):
        c=False; rl=0; h=[0]*7
        for x in range(size):
            if m[y][x]==c:
                rl+=1
                if rl==5: res+=N1
                elif rl>5: res+=1
            else:
                add_hist(rl,h)
                if not c: res+=count_pat(h)*N3
                c=m[y][x]; rl=1
        res+=term(c,rl,h)*N3
    # 列
    for x in range(size):
        c=False; rl=0; h=[0]*7
        for y in range(size):
            if m[y][x]==c:
                rl+=1
                if rl==5: res+=N1
                elif rl>5: res+=1
            else:
                add_hist(rl,h)
                if not c: res+=count_pat(h)*N3
                c=m[y][x]; rl=1
        res+=term(c,rl,h)*N3
    # 2x2 同色块
    for y in range(size-1):
        for x in range(size-1):
            v=m[y][x]
            if v==m[y][x+1] and v==m[y+1][x] and v==m[y+1][x+1]: res+=N2
    # 深色比例
    dark=sum(1 for y in range(size) for x in range(size) if m[y][x])
    total=size*size
    import math
    k=math.ceil(abs(dark*20-total*10)/total)-1
    res+=max(0,k)*N4
    return res

def js_all(text,ecc):
    '''runall.js 一次给出 8 个掩码的矩阵'''
    o=subprocess.run([NODE,'runall.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0: raise RuntimeError(o.stderr.decode('utf-8','replace'))
    d=json.loads(o.stdout.decode())
    return d['version'], d['masks']

def js_chosen(text,ecc):
    o=subprocess.run([NODE,'run.js',text,ecc],capture_output=True,cwd=HERE)
    return json.loads(o.stdout.decode())['mask']

def js_pen(m):
    '''把矩阵喂给 JS 的 penaltyOf，取回它算的罚分'''
    o=subprocess.run([NODE,'pen_same.js'],input=json.dumps(m).encode(),
                     capture_output=True,cwd=HERE)
    if o.returncode!=0: raise RuntimeError(o.stderr.decode('utf-8','replace'))
    return int(o.stdout.decode())

cases=[('https://orcode.example/long?'+'z'*1200,'L'),
       ('https://a.b/c?d='+'y'*600,'M'),
       ('HELLO WORLD','M'),
       ('中文二维码测试','Q'),
       ('https://music.163.com/login?codekey=abc123','H')]

allok=True
for text,ecc in cases:
    ver, mats = js_all(text,ecc)
    chosen = js_chosen(text,ecc)
    py=[]
    for mask in range(8):
        m = [[bool(c) for c in row] for row in mats[mask]]
        got = js_pen(m)
        exp = penalty(m)
        if got != exp:
            allok = False
            print('  mask=%d  JS=%d  Py=%d  <-- 不一致' % (mask, got, exp))
        py.append(exp)
    best_py = py.index(min(py))
    ok = (chosen == best_py)
    if not ok: allok = False
    print('%-32s ecc=%s v%-3d JS选定=%d  最小罚分应=%d  %s'
          % (repr(text[:30]), ecc, ver, chosen, best_py, '✓' if ok else '✗'))
print()
print('JS 与 Python 独立实现罚分完全一致 ✓' if allok else '存在差异 ✗')
sys.exit(0 if allok else 1)
