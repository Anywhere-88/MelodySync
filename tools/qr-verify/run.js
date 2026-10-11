var QR = require('./qrlite_verify.js');
var text = process.argv[2], ecc = process.argv[3] || 'M';
var fm = (process.argv[4] === undefined || process.argv[4] === '') ? undefined : Number(process.argv[4]);
var qr = QR.encode(text, ecc, fm);
process.stdout.write(JSON.stringify({
  version: qr.version, mask: qr.mask, size: qr.size,
  modules: qr.modules.map(function (r) { return r.map(function (v) { return v ? 1 : 0; }); })
}));
