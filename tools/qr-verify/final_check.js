// 正式版 vs 测试副本：同内容自动掩码结果必须完全一致
var CLEAN = require('./qrlite.js'), VER = require('./qrlite_verify.js');
var ok = true;
['a','https://music.163.com/login?codekey='+'a1b2c3d4e5f6'.repeat(3),'中文二维码测试']
.forEach(function(t){
  ['L','M','Q','H'].forEach(function(e){
    var a = CLEAN.encode(t,e), b = VER.encode(t,e);
    var same = a.size===b.size && a.mask===b.mask;
    if (same) for (var y=0;y<a.size;y++) for (var x=0;x<a.size;x++)
      if (a.modules[y][x]!==b.modules[y][x]) same=false;
    if (!same) { ok=false; console.log('✗ 不一致', e, t.slice(0,20)); }
  });
});
console.log(ok ? '正式版与测试副本输出完全一致 ✓' : '存在差异 ✗');
