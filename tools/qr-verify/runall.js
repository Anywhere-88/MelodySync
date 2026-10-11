var QR = require('./qrlite_verify.js');
var text = process.argv[2], ecc = process.argv[3] || 'M';
var out = { version: null, size: 0, masks: [], auto: null };
for (var m = 0; m < 8; m++) {
  var qr = QR.encode(text, ecc, m);
  out.version = qr.version; out.size = qr.size;
  out.masks.push(qr.modules.map(function (r) { return r.map(function (v) { return v ? 1 : 0; }); }));
}
process.stdout.write(JSON.stringify(out));
