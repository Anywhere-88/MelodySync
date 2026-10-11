# -*- coding: utf-8 -*-
"""
生成 qrlite_verify.js —— 仅供测试使用的一份副本。

正式版 qrlite.js 不暴露「强制指定掩码」的开关（生产代码不需要），
但逐模块比对参考实现时必须能固定同一个掩码，否则两边选的掩码不同，
矩阵自然不一样。本脚本把该开关临时加回，产出 qrlite_verify.js。
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

src = open(os.path.join(HERE, 'qrlite.js'), encoding='utf-8').read()

v = src.replace(
    "  function buildMatrix(version, ecl, codewords, eclName) {",
    "  function buildMatrix(version, ecl, codewords, eclName, forceMask) {")
v = v.replace(
    """    // 8 个掩码逐个试，取 ISO 罚分最小者
    var bestMask = 0, minPenalty = Infinity;""",
    """    if (forceMask !== undefined && forceMask !== null) {
      applyMask(forceMask); drawFormat(forceMask);
      return { size: size, version: version, modules: mods, mask: forceMask };
    }
    // 8 个掩码逐个试，取 ISO 罚分最小者
    var bestMask = 0, minPenalty = Infinity;""")
v = v.replace("  function encode(text, eccLevel) {",
              "  function encode(text, eccLevel, forceMask) {")
v = v.replace("    return buildMatrix(cw.version, ecl, cw.codewords, name);\n  }\n\n  /* 转成 SVG",
              "    return buildMatrix(cw.version, ecl, cw.codewords, name, forceMask);\n  }\n\n  /* 转成 SVG")

assert v != src, '未匹配到需要替换的代码，请检查 qrlite.js 是否被改动过'
assert 'forceMask' in v

out = os.path.join(HERE, 'qrlite_verify.js')
open(out, 'w', encoding='utf-8').write(v)
print('已生成', out)
