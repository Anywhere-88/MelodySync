/* ============================================================================
 *  QRCodeLite —— 极小 QR Code 编码器（纯 JS，零依赖，离线可用）
 *
 *  为什么不用 CDN 的 qrcode.js：本程序是本地桌面应用，界面层通过 qrc 打进 exe，
 *  运行时不允许也不会去联网拉脚本。所以二维码必须自己生成。
 *
 *  支持范围：byte 模式（UTF-8）、版本 1-40、纠错等级 L/M/Q/H、掩码自动优选。
 *  实现严格遵循 ISO/IEC 18004：
 *    1) 选版本 → 2) 拼位流 → 3) 补齐 → 4) 分块 RS 纠错 → 5) 交错
 *    → 6) 布点（定位/校正/时序/格式/版本图形）→ 7) 掩码优选 → 8) 写格式信息
 * ========================================================================== */
var QRCodeLite = (function () {
  'use strict';

  /* 每块纠错码字数，索引 [ecl][version]（0 位占位） */
  var ECC_CODEWORDS_PER_BLOCK = [
    [-1,7,10,15,20,26,18,20,24,30,18,20,24,26,30,22,24,28,30,28,28,28,28,30,30,26,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
    [-1,10,16,26,18,24,16,18,22,22,26,30,22,22,24,24,28,28,26,26,26,26,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28],
    [-1,13,22,18,26,18,24,18,22,20,24,28,26,24,20,30,24,28,28,26,30,28,30,30,30,30,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
    [-1,17,28,22,16,22,28,26,26,24,28,24,28,22,24,24,30,28,28,26,28,30,24,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30]
  ];
  /* 纠错块数，索引同上 */
  var NUM_ERROR_CORRECTION_BLOCKS = [
    [-1,1,1,1,1,1,2,2,2,2,4,4,4,4,4,6,6,6,6,7,8,8,9,9,10,12,12,12,13,14,15,16,17,18,19,19,20,21,22,24,25],
    [-1,1,1,1,2,2,4,4,4,5,5,5,8,9,9,10,10,11,13,14,16,17,17,18,20,21,23,25,26,28,29,31,33,35,37,38,40,43,45,47,49],
    [-1,1,1,2,2,4,4,6,6,8,8,8,10,12,16,12,17,16,18,21,20,23,23,25,27,29,34,34,35,38,40,43,45,48,51,53,56,59,62,65,68],
    [-1,1,1,2,4,4,4,5,6,8,8,11,11,16,16,18,16,19,21,25,25,25,34,30,32,35,37,40,42,45,48,51,54,57,60,63,66,70,74,77,81]
  ];

  /* 格式信息里的 2 位纠错指示符（注意不是 0..3 的顺序） */
  var FORMAT_BITS = { L: 1, M: 0, Q: 3, H: 2 };
  var ECL_INDEX   = { L: 0, M: 1, Q: 2, H: 3 };

  var PENALTY_N1 = 3, PENALTY_N2 = 3, PENALTY_N3 = 40, PENALTY_N4 = 10;

  /* -------------------- GF(256) 运算（本原多项式 0x11D） ------------------- */
  var EXP = new Uint8Array(512);
  var LOG = new Uint8Array(256);
  (function () {
    var x = 1;
    for (var i = 0; i < 255; i++) {
      EXP[i] = x;
      LOG[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    for (var j = 255; j < 512; j++) EXP[j] = EXP[j - 255];
  })();

  function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return EXP[LOG[a] + LOG[b]];
  }

  /* 生成多项式 ∏(x - a^i)，降序系数（最高次项隐式为 1） */
  function rsDivisor(degree) {
    var result = new Array(degree);
    for (var i = 0; i < degree; i++) result[i] = 0;
    result[degree - 1] = 1;
    var root = 1;
    for (var i2 = 0; i2 < degree; i2++) {
      for (var j = 0; j < result.length; j++) {
        result[j] = gfMul(result[j], root);
        if (j + 1 < result.length) result[j] ^= result[j + 1];
      }
      root = gfMul(root, 0x02);
    }
    return result;
  }

  /* 多项式除法取余数 = 该块的纠错码字 */
  function rsRemainder(data, divisor) {
    var result = new Array(divisor.length);
    for (var i = 0; i < result.length; i++) result[i] = 0;
    for (var k = 0; k < data.length; k++) {
      var factor = data[k] ^ result.shift();
      result.push(0);
      for (var j = 0; j < divisor.length; j++) result[j] ^= gfMul(divisor[j], factor);
    }
    return result;
  }

  /* ------------------------------ 容量计算 ------------------------------- */
  function rawDataModules(ver) {
    var n = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
      var numAlign = Math.floor(ver / 7) + 2;
      n -= (25 * numAlign - 10) * numAlign - 55;
      if (ver >= 7) n -= 36;
    }
    return n;
  }
  function dataCodewordCount(ver, ecl) {
    return Math.floor(rawDataModules(ver) / 8)
         - ECC_CODEWORDS_PER_BLOCK[ecl][ver] * NUM_ERROR_CORRECTION_BLOCKS[ecl][ver];
  }
  function charCountBits(ver) { return ver <= 9 ? 8 : 16; }

  function utf8Bytes(str) {
    var out = [];
    for (var i = 0; i < str.length; i++) {
      var c = str.charCodeAt(i);
      if (c < 0x80) { out.push(c); }
      else if (c < 0x800) { out.push(0xC0 | (c >> 6), 0x80 | (c & 63)); }
      else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
        var c2 = str.charCodeAt(++i);
        var cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
        out.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 63),
                 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
      } else { out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); }
    }
    return out;
  }

  /* ------------------------------ 位流拼装 ------------------------------- */
  function buildCodewords(bytes, ecl) {
    // 1) 选最小可用版本
    var ver = -1;
    for (var v = 1; v <= 40; v++) {
      var cap = dataCodewordCount(v, ecl) * 8;
      var need = 4 + charCountBits(v) + 8 * bytes.length;
      if (need <= cap) { ver = v; break; }
    }
    if (ver < 0) throw new Error('数据过长，超出 QR 版本 40 的容量');

    var bits = [];
    function push(val, len) { for (var i = len - 1; i >= 0; i--) bits.push((val >>> i) & 1); }

    push(0x4, 4);                       // 模式指示符：0100 = byte
    push(bytes.length, charCountBits(ver));
    for (var i = 0; i < bytes.length; i++) push(bytes[i], 8);

    var capacityBits = dataCodewordCount(ver, ecl) * 8;
    push(0, Math.min(4, capacityBits - bits.length));      // 终止符
    push(0, (8 - bits.length % 8) % 8);                    // 补齐到字节边界

    var codewords = [];
    for (var b = 0; b < bits.length; b += 8) {
      var byteVal = 0;
      for (var k = 0; k < 8; k++) byteVal = (byteVal << 1) | bits[b + k];
      codewords.push(byteVal);
    }
    // 交替填充 0xEC / 0x11
    for (var pad = 0xEC; codewords.length < dataCodewordCount(ver, ecl); pad ^= 0xEC ^ 0x11)
      codewords.push(pad);

    // 2) 按块纠错 + 交错
    var numBlocks = NUM_ERROR_CORRECTION_BLOCKS[ecl][ver];
    var eccLen    = ECC_CODEWORDS_PER_BLOCK[ecl][ver];
    var rawCodewords = Math.floor(rawDataModules(ver) / 8);
    var shortBlockLen = Math.floor(rawCodewords / numBlocks) - eccLen;
    var numShort = numBlocks - rawCodewords % numBlocks;   // 前 numShort 块短 1 字节

    var divisor = rsDivisor(eccLen);
    var blocks = [];
    for (var bi = 0, off = 0; bi < numBlocks; bi++) {
      var len = shortBlockLen + (bi < numShort ? 0 : 1);
      var dat = codewords.slice(off, off + len);
      off += len;
      blocks.push({ data: dat, ecc: rsRemainder(dat, divisor) });
    }

    var result = [];
    for (var idx = 0; idx < shortBlockLen + 1; idx++)
      for (var bj = 0; bj < numBlocks; bj++)
        if (idx !== shortBlockLen || bj >= numShort) result.push(blocks[bj].data[idx]);
    for (var ei = 0; ei < eccLen; ei++)
      for (var bk = 0; bk < numBlocks; bk++) result.push(blocks[bk].ecc[ei]);

    return { version: ver, codewords: result, rawCodewords: rawCodewords };
  }

  /* --------------------- ISO 18004 8.8.2 掩码罚分 ------------------------ */
  function penaltyOf(mods) {
    var size = mods.length;
    var res = 0, y, x;
    function addHistory(run, hist) {
      if (hist[0] === 0) run += size;      // 行/列开头若为浅色，需补上边界外的空白
      hist.pop(); hist.unshift(run);
    }
    function countPatterns(hist) {
      var n = hist[1];
      var core = n > 0 && hist[2] === n && hist[3] === n * 3 &&
                 hist[4] === n && hist[5] === n;
      return (core && hist[0] >= n * 4 && hist[6] >= n ? 1 : 0)
           + (core && hist[6] >= n * 4 && hist[0] >= n ? 1 : 0);
    }
    function terminateAndCount(runColor, runLen, hist) {
      if (runColor) { addHistory(runLen, hist); runLen = 0; }
      runLen += size;
      addHistory(runLen, hist);
      return countPatterns(hist);
    }
    for (y = 0; y < size; y++) {
      var rc = false, runX = 0, hist = [0, 0, 0, 0, 0, 0, 0];
      for (x = 0; x < size; x++) {
        if (mods[y][x] === rc) {
          runX++;
          if (runX === 5) res += PENALTY_N1;
          else if (runX > 5) res++;
        } else {
          addHistory(runX, hist);
          if (!rc) res += countPatterns(hist) * PENALTY_N3;
          rc = mods[y][x]; runX = 1;
        }
      }
      res += terminateAndCount(rc, runX, hist) * PENALTY_N3;
    }
    for (x = 0; x < size; x++) {
      var rc2 = false, runY = 0, hist2 = [0, 0, 0, 0, 0, 0, 0];
      for (y = 0; y < size; y++) {
        if (mods[y][x] === rc2) {
          runY++;
          if (runY === 5) res += PENALTY_N1;
          else if (runY > 5) res++;
        } else {
          addHistory(runY, hist2);
          if (!rc2) res += countPatterns(hist2) * PENALTY_N3;
          rc2 = mods[y][x]; runY = 1;
        }
      }
      res += terminateAndCount(rc2, runY, hist2) * PENALTY_N3;
    }
    for (y = 0; y < size - 1; y++)
      for (x = 0; x < size - 1; x++) {
        var c = mods[y][x];
        if (c === mods[y][x + 1] && c === mods[y + 1][x] && c === mods[y + 1][x + 1])
          res += PENALTY_N2;
      }
    var dark = 0;
    for (y = 0; y < size; y++) for (x = 0; x < size; x++) if (mods[y][x]) dark++;
    var total = size * size;
    var kk = Math.ceil(Math.abs(dark * 20 - total * 10) / total) - 1;
    res += kk * PENALTY_N4;
    return res;
  }

  /* ------------------------------ 矩阵绘制 ------------------------------- */
  function AlignmentPositions(ver) {
    if (ver === 1) return [];
    var numAlign = Math.floor(ver / 7) + 2;
    var size = ver * 4 + 17;
    var step = (ver === 32) ? 26
             : Math.ceil((ver * 4 + 4) / (numAlign * 2 - 2)) * 2;
    var result = [6];
    for (var pos = size - 7; result.length < numAlign; pos -= step) result.splice(1, 0, pos);
    return result;
  }

  function buildMatrix(version, ecl, codewords, eclName) {
    var size = version * 4 + 17;
    var mods = [], func = [];
    for (var y = 0; y < size; y++) {
      mods.push(new Array(size)); func.push(new Array(size));
      for (var x = 0; x < size; x++) { mods[y][x] = false; func[y][x] = false; }
    }

    function setFn(x, y, dark) {
      if (x < 0 || y < 0 || x >= size || y >= size) return;
      mods[y][x] = !!dark; func[y][x] = true;
    }
    function getBit(v, i) { return ((v >>> i) & 1) !== 0; }

    // --- 时序图形（必须先画：定位图形随后会覆盖它的两端）
    for (var i = 0; i < size; i++) {
      setFn(6, i, i % 2 === 0);
      setFn(i, 6, i % 2 === 0);
    }

    // --- 定位图形 + 分隔带（覆盖时序图形两端，顺序不能反）
    function drawFinder(cx, cy) {
      for (var dy = -4; dy <= 4; dy++)
        for (var dx = -4; dx <= 4; dx++) {
          var dist = Math.max(Math.abs(dx), Math.abs(dy));
          setFn(cx + dx, cy + dy, dist !== 2 && dist !== 4);
        }
    }
    drawFinder(3, 3); drawFinder(size - 4, 3); drawFinder(3, size - 4);

    // --- 校正图形
    var aligns = AlignmentPositions(version);
    for (var ai = 0; ai < aligns.length; ai++)
      for (var aj = 0; aj < aligns.length; aj++) {
        if ((ai === 0 && aj === 0) ||
            (ai === 0 && aj === aligns.length - 1) ||
            (ai === aligns.length - 1 && aj === 0)) continue;
        for (var dy2 = -2; dy2 <= 2; dy2++)
          for (var dx2 = -2; dx2 <= 2; dx2++)
            setFn(aligns[ai] + dx2, aligns[aj] + dy2,
                  Math.max(Math.abs(dx2), Math.abs(dy2)) !== 1);
      }

    // --- 格式信息（先按掩码 0 占位，最后会重画）
    function drawFormat(mask) {
      var data = (FORMAT_BITS[eclName] << 3) | mask;
      var rem = data;
      for (var k = 0; k < 10; k++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
      var bits = ((data << 10) | rem) ^ 0x5412;
      for (var i2 = 0; i2 <= 5; i2++) setFn(8, i2, getBit(bits, i2));
      setFn(8, 7, getBit(bits, 6));
      setFn(8, 8, getBit(bits, 7));
      setFn(7, 8, getBit(bits, 8));
      for (var i3 = 9; i3 < 15; i3++) setFn(14 - i3, 8, getBit(bits, i3));
      for (var i4 = 0; i4 < 8; i4++) setFn(size - 1 - i4, 8, getBit(bits, i4));
      for (var i5 = 8; i5 < 15; i5++) setFn(8, size - 15 + i5, getBit(bits, i5));
      setFn(8, size - 8, true);          // 固定深色模块
    }

    // --- 版本信息（版本 >= 7 才有）
    if (version >= 7) {
      var vrem = version;
      for (var i6 = 0; i6 < 12; i6++) vrem = (vrem << 1) ^ ((vrem >>> 11) * 0x1F25);
      var vbits = (version << 12) | vrem;
      for (var i7 = 0; i7 < 18; i7++) {
        var bit = getBit(vbits, i7);
        var aa = size - 11 + i7 % 3, bb = Math.floor(i7 / 3);
        setFn(aa, bb, bit); setFn(bb, aa, bit);
      }
    }

    drawFormat(0);

    // --- 数据布点（右下起，两列一组蛇形上行/下行）
    var bitIndex = 0, totalBits = codewords.length * 8;
    for (var right = size - 1; right >= 1; right -= 2) {
      if (right === 6) right = 5;                       // 跳过时序列
      for (var vert = 0; vert < size; vert++) {
        for (var j = 0; j < 2; j++) {
          var x2 = right - j;
          var upward = ((right + 1) & 2) === 0;
          var y2 = upward ? size - 1 - vert : vert;
          if (!func[y2][x2] && bitIndex < totalBits) {
            mods[y2][x2] = getBit(codewords[bitIndex >>> 3], 7 - (bitIndex & 7));
            bitIndex++;
          }
        }
      }
    }

    // --- 掩码
    function applyMask(mask) {
      for (var y3 = 0; y3 < size; y3++)
        for (var x3 = 0; x3 < size; x3++) {
          if (func[y3][x3]) continue;
          var inv;
          switch (mask) {
            case 0: inv = (x3 + y3) % 2 === 0; break;
            case 1: inv = y3 % 2 === 0; break;
            case 2: inv = x3 % 3 === 0; break;
            case 3: inv = (x3 + y3) % 3 === 0; break;
            case 4: inv = (Math.floor(x3 / 3) + Math.floor(y3 / 2)) % 2 === 0; break;
            case 5: inv = x3 * y3 % 2 + x3 * y3 % 3 === 0; break;
            case 6: inv = (x3 * y3 % 2 + x3 * y3 % 3) % 2 === 0; break;
            case 7: inv = ((x3 + y3) % 2 + x3 * y3 % 3) % 2 === 0; break;
          }
          if (inv) mods[y3][x3] = !mods[y3][x3];
        }
    }

    function penalty() { return penaltyOf(mods); }

    // 8 个掩码逐个试，取 ISO 罚分最小者
    var bestMask = 0, minPenalty = Infinity;
    for (var mask = 0; mask < 8; mask++) {
      applyMask(mask);
      drawFormat(mask);
      var p = penalty();
      if (p < minPenalty) { minPenalty = p; bestMask = mask; }
      applyMask(mask);                  // 撤销，试下一个
    }
    applyMask(bestMask);
    drawFormat(bestMask);

    return { size: size, version: version, modules: mods, mask: bestMask };
  }

  /* ------------------------------- 对外接口 ------------------------------ */
  function encode(text, eccLevel) {
    var name = (eccLevel && FORMAT_BITS[eccLevel] !== undefined) ? eccLevel : 'M';
    var ecl = ECL_INDEX[name];
    var bytes = utf8Bytes(String(text));
    var cw = buildCodewords(bytes, ecl);
    return buildMatrix(cw.version, ecl, cw.codewords, name);
  }

  /* 转成 SVG 字符串（viewBox 单位 = 模块数，缩放交给 CSS） */
  function toSvg(text, opts) {
    opts = opts || {};
    var ecc = opts.ecc || 'M';
    var quiet = opts.quiet === undefined ? 2 : opts.quiet;
    var dark = opts.dark || '#0d0f14';
    var light = opts.light || '#ffffff';
    var qr = encode(text, ecc);
    var n = qr.size + quiet * 2;
    var path = [];
    for (var y = 0; y < qr.size; y++)
      for (var x = 0; x < qr.size; x++)
        if (qr.modules[y][x]) path.push('M' + (x + quiet) + ' ' + (y + quiet) + 'h1v1h-1z');
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + n + ' ' + n +
           '" shape-rendering="crispEdges" style="width:100%;height:100%;display:block">' +
           '<rect width="' + n + '" height="' + n + '" fill="' + light + '"/>' +
           '<path d="' + path.join('') + '" fill="' + dark + '"/></svg>';
  }

  /* penaltyOf 也对外暴露：便于用测试脚本校验掩码罚分是否与 ISO 一致 */
  return { encode: encode, toSvg: toSvg, penaltyOf: penaltyOf, version: '1.0' };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = QRCodeLite;
