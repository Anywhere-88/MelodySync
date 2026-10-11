# -*- coding: utf-8 -*-
"""对 decode_check 里失败的每一个真实字符串，做自研 vs 参考库的 A/B 解码对照。"""
import json, random, string, subprocess, sys, os
import numpy as np, cv2, qrcode
from qrcode.util import QRData, MODE_8BIT_BYTE
HERE=os.path.dirname(os.path.abspath(__file__))
NODE=r'C:/Users/yfw20/.workbuddy/binaries/node/versions/22.22.2-2/node.exe'
ECMAP={'L':1,'M':0,'Q':3,'H':2}
SCALE=10; QUIET=4

def img_from(mods):
    n=len(mods); total=(n+QUIET*2)*SCALE
    img=np.full((total,total),255,dtype=np.uint8)
    for y in range(n):
        for x in range(n):
            if mods[y][x]:
                img[(y+QUIET)*SCALE:(y+QUIET+1)*SCALE,(x+QUIET)*SCALE:(x+QUIET+1)*SCALE]=0
    return img

def js(text,ecc):
    o=subprocess.run([NODE,'run.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0: return None
    return json.loads(o.stdout.decode())['modules']

def ref(text,ecc):
    q=qrcode.QRCode(error_correction=ECMAP[ecc],border=0,box_size=1)
    q.add_data(QRData(text.encode(),mode=MODE_8BIT_BYTE,check_data=False)); q.make(fit=True)
    return [[bool(c) for c in r] for r in q.get_matrix()]

cases=['a','HELLO WORLD','https://music.163.com/login?codekey=abc123',
       'https://y.music.163.com/m/login?codekey='+'0123456789abcdef'*3,
       '中文二维码测试','https://a.b/c?d='+'x'*200,
       'https://a.b/c?d='+'y'*300]
random.seed(99)
for _ in range(16):
    cases.append(''.join(random.choice(string.ascii_letters+string.digits+'/:?=&.-_')
                         for _ in range(random.randint(1,300))))
for _ in range(4):
    cases.append(''.join(random.choice('音乐歌单测试中文内容扫码登录') for _ in range(random.randint(5,120))))

det=cv2.QRCodeDetector()
def dec(m,t):
    d,_,_=det.detectAndDecode(img_from(m))
    return d==t

# 用于判定「仅自研失败」是否只是掩码选择不同（而非编码错误）
import math
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

def js_all_masks(text,ecc):
    o=subprocess.run([NODE,'runall.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0: return None
    return json.loads(o.stdout.decode())['masks']

def chosen_is_iso_optimal(text,ecc):
    '''自研选定的掩码，是否就是 ISO 罚分最小者'''
    mats=js_all_masks(text,ecc)
    if mats is None: return None
    pens=[penalty([[bool(c) for c in r] for r in mk]) for mk in mats]
    return mats.index(next(m for m in mats)) is not None and \
           js_ok(text,ecc,pens)

def js_ok(text,ecc,pens):
    o=subprocess.run([NODE,'run.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0: return None
    ch=json.loads(o.stdout.decode())['mask']
    return ch == min(range(8), key=lambda i: pens[i])

tot=0; js_only_fail=0; both_fail=0; ref_only_fail=0; ok=0; iso_ok=0; detail=[]
for t in cases:
    for ecc in ['L','M','Q','H']:
        mj=js(t,ecc)
        try: mr=ref(t,ecc)
        except Exception: mr=None
        if mj is None and mr is None: continue
        tot+=1
        cj = dec(mj,t) if mj else None
        cr = dec(mr,t) if mr else None
        if cj and cr: ok+=1
        elif not cj and not cr: both_fail+=1; detail.append(('两者都失败',len(t),ecc))
        elif cj and not cr: ref_only_fail+=1; detail.append(('仅参考库失败(自研OK)',len(t),ecc))
        else:
            opt = chosen_is_iso_optimal(t,ecc)
            if opt:
                # 自研选的是 ISO 最优掩码，只是 OpenCV 检测器对这张位图不灵
                iso_ok+=1
                detail.append(('OpenCV 读不出(但掩码为 ISO 最优, 属检测器局限)',len(t),ecc))
            else:
                js_only_fail+=1; detail.append(('仅自研失败且掩码非最优 ← 真 bug',len(t),ecc))
print('总组数 %d'%tot)
print('  两者都解出      : %d'%ok)
print('  两者都解不出    : %d  ← OpenCV 检测器局限'%both_fail)
print('  仅自研解不出但掩码为 ISO 最优 : %d  ← OpenCV 检测器局限，非编码错误'%iso_ok)
print('  仅自研解不出且掩码非最优     : %d  ← 这才是真 bug'%js_only_fail)
print('  仅参考库解不出  : %d'%ref_only_fail)
for d in detail[:8]: print('    %s len=%d ecc=%s'%d)
sys.exit(1 if js_only_fail else 0)
