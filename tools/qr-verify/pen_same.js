// 对一个给定矩阵在 JS 侧计算 ISO 掩码罚分（从 stdin 读矩阵 JSON）
var QR = require('./qrlite.js');
var buf = '';
process.stdin.on('data', function (c) { buf += c; });
process.stdin.on('end', function () {
  var m = JSON.parse(buf).map(function (r) {
    return r.map(function (v) { return !!v; });
  });
  process.stdout.write(String(QR.penaltyOf(m)));
});
