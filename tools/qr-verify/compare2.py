# -*- coding: utf-8 -*-
"""固定同一掩码，逐模块比对；分离"掩码优选差异"与"布点/纠错差异"。"""
import json, random, string, subprocess, sys, os
import qrcode
from qrcode.util import QRData, MODE_8BIT_BYTE
HERE=os.path.dirname(os.path.abspath(__file__))
NODE=r'C:/Users/yfw20/.workbuddy/binaries/node/versions/22.22.2-2/node.exe'
ECMAP={'L':1,'M':0,'Q':3,'H':2}

def ref(text, ecc, mask):
    q=qrcode.QRCode(error_correction=ECMAP[ecc],border=0,box_size=1)
    q.add_data(QRData(text.encode(),mode=MODE_8BIT_BYTE,check_data=False))
    q.best_fit()
    q.makeImpl(False, mask)
    return [[bool(c) for c in r] for r in q.get_matrix()]

def js_all(text, ecc):
    o=subprocess.run([NODE,'runall.js',text,ecc],capture_output=True,cwd=HERE)
    if o.returncode!=0: raise RuntimeError(o.stderr.decode('utf-8','replace'))
    d=json.loads(o.stdout.decode())
    return [[[bool(c) for c in r] for r in mk] for mk in d['masks']]

cases=['a','HELLO','https://music.163.com/login?codekey=abc123',
       'https://a.b/c?d='+'x'*100, 'https://a.b/c?d='+'y'*400,
       '中文二维码测试','https://a.b/c?d='+'z'*900]
random.seed(7)
for _ in range(20):
    cases.append(''.join(random.choice(string.ascii_letters+string.digits+'/:?=&.-_')
                         for _ in range(random.randint(1,900))))

total=0; bad=[]
for t in cases:
    for ecc in ['L','M','Q','H']:
        try: r0=ref(t,ecc,0)
        except Exception: continue
        gs=js_all(t,ecc)
        for mask in range(8):
            r=ref(t,ecc,mask); g=gs[mask]
            total+=1
            if len(r)!=len(g): bad.append((len(t),ecc,mask,'size')); continue
            diff=sum(1 for y in range(len(r)) for x in range(len(r)) if r[y][x]!=g[y][x])
            if diff: bad.append((len(t),ecc,mask,'%d 模块不一致'%diff))
print('比对 %d 组'%total)
if bad:
    print('不一致 %d 组，前 12:'%len(bad))
    for b in bad[:12]: print('   len=%-5d ecc=%s mask=%d  %s'%b)
    sys.exit(1)
print('同一掩码下逐模块完全一致 ✓ —— 说明纠错/布点/格式信息全部正确，')
print('之前的差异只来自"掩码优选"的罚分算法。')
