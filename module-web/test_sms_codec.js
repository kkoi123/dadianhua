'use strict';
const assert = require('node:assert/strict');
const codec = require('./static/sms-codec.js');
assert.equal(codec.encode('+8613800138000', '你好', 1)[0], '0001000d91683108108300f00008044f60597d');
const pdu = '00000d91683108108300f0000862013010203023044f60597d';
const decoded = codec.decode(pdu);
assert.equal(decoded.number, '+8613800138000');assert.equal(decoded.body, '你好');assert.equal(decoded.timestamp, Date.UTC(2026,9,2,17,2,3));
const gsm = codec.decode('00000d91683108108300f000006201301020302305e8329bfd06');assert.equal(gsm.body, 'hello');
for(const body of ['a'.repeat(70), '中'.repeat(71), 'A'.repeat(66)+'😀'+'B'.repeat(80), '😀'.repeat(200)]) {
  const parts=codec.encode('13800138000',body,123);assert.equal(parts.map(x=>codec.decode(x).body).join(''),body);
  if(parts.length>1)parts.forEach((p,i)=>{const c=codec.decode(p).concat;assert.equal(c.ref,123);assert.equal(c.total,parts.length);assert.equal(c.seq,i+1);});
}
assert.throws(()=>codec.decode(pdu.slice(0,-2)));assert.throws(()=>codec.encode('13800138000','\ud800',1));assert.throws(()=>codec.encode('13800138000','x'.repeat(671),1));
console.log('SMS codec checks passed: golden UCS2/GSM7 PDUs, timestamp, multipart Unicode and malformed inputs.');
