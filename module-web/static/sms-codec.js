/* 3GPP SMS PDU codec. No network or storage operations. */
(() => {
  'use strict';
  const alphabet = ['@','£','$','¥','è','é','ù','ì','ò','Ç','\n','Ø','ø','\r','Å','å','Δ','_','Φ','Γ','Λ','Ω','Π','Ψ','Σ','Θ','Ξ','','Æ','æ','ß','É',' ','!','"','#','¤','%','&',"'",'(',')','*','+',',','-','.','/','0','1','2','3','4','5','6','7','8','9',':',';','<','=','>','?','¡',...'ABCDEFGHIJKLMNOPQRSTUVWXYZ','Ä','Ö','Ñ','Ü','§','¿',...'abcdefghijklmnopqrstuvwxyz','ä','ö','ñ','ü','à'];
  const extensions = {10:'\f',20:'^',40:'{',41:'}',47:'\\',60:'[',61:'~',62:']',64:'|',101:'€'};
  const hex = (bytes) => bytes.map(x => x.toString(16).padStart(2, '0')).join('');
  function gsm(bytes, count, offset = 0) {
    let result = '', escaped = false;
    for (let i = 0; i < count; i++) {
      const bit = offset + i * 7, at = Math.floor(bit / 8), shift = bit % 8;
      if (bit + 7 > bytes.length * 8) throw new Error('短信数据不完整');
      const c = ((bytes[at] >> shift) | ((bytes[at + 1] || 0) << (8 - shift))) & 127;
      if (escaped) { result += extensions[c] || '�'; escaped = false; }
      else if (c === 27) escaped = true;
      else result += alphabet[c];
    }
    return result;
  }
  function encode(number, body, reference) {
    number = number.trim().replace(/[\s()-]/g, '');
    if (!/^\+?\d{1,20}$/.test(number)) throw new Error('请输入正确的收件号码');
    if (!body.trim() || body.length > 670) throw new Error('请输入短信内容，最多 670 个 UTF-16 字符');
    for (let i = 0; i < body.length; i++) {
      const u = body.charCodeAt(i);
      if (u >= 0xd800 && u <= 0xdbff) { const next = body.charCodeAt(++i); if (!(next >= 0xdc00 && next <= 0xdfff)) throw new Error('短信含有无效字符'); }
      else if (u >= 0xdc00 && u <= 0xdfff) throw new Error('短信含有无效字符');
    }
    const limit = body.length <= 70 ? 70 : 67, chunks = [];
    for (let at = 0; at < body.length;) {
      let end = Math.min(at + limit, body.length);
      if (end < body.length && body.charCodeAt(end - 1) >= 0xd800 && body.charCodeAt(end - 1) <= 0xdbff) end--;
      chunks.push(body.slice(at, end)); at = end;
    }
    if (chunks.length > 10) throw new Error('短信分段过多，请缩短内容');
    const digits = number.replace(/^\+/, ''), address = [];
    for (let i = 0; i < digits.length; i += 2) address.push(Number(digits[i]) | ((i + 1 < digits.length ? Number(digits[i + 1]) : 15) << 4));
    return chunks.map((chunk, i) => {
      const data = chunks.length > 1 ? [5, 0, 3, reference & 255, chunks.length, i + 1] : [];
      for (let j = 0; j < chunk.length; j++) { const u = chunk.charCodeAt(j); data.push(u >> 8, u & 255); }
      return hex([0, chunks.length > 1 ? 0x41 : 1, 0, digits.length, number.startsWith('+') ? 0x91 : 0x81, ...address, 0, 8, data.length, ...data]);
    });
  }
  function decode(raw) {
    if (!/^(?:[a-f0-9]{2})+$/i.test(raw)) throw new Error('短信编码无效');
    const p = raw.match(/../g).map(x => parseInt(x, 16)); let at = 0;
    function take(n) { if (!Number.isInteger(n) || n < 0 || at + n > p.length) throw new Error('短信数据不完整'); const b = p.slice(at, at + n); at += n; return b; }
    const byte = () => take(1)[0];
    take(byte()); const first = byte(), kind = first & 3;
    if (kind !== 0 && kind !== 1) throw new Error('此短信类型暂不支持');
    if (kind === 1) byte();
    const length = byte(), type = byte(), address = take(Math.ceil(length / 2));
    let number = '';
    if ((type & 0x70) === 0x50) number = gsm(address, Math.floor(length * 4 / 7));
    else { if ((type & 0x70) === 0x10) number = '+'; for (let i = 0; i < length; i++) { const n = (address[i >> 1] >> ((i % 2) * 4)) & 15; number += n < 10 ? n : '�'; } }
    byte(); const dcs = byte(); let timestamp = null;
    if (kind === 0) {
      const t = take(7), digit = x => (x & 15) * 10 + (x >> 4), v = t.slice(0, 6).map(digit);
      const year = (v[0] >= 70 ? 1900 : 2000) + v[0];
      const date = new Date(Date.UTC(year, v[1] - 1, v[2], v[3], v[4], v[5]));
      if (date.getUTCFullYear() === year && date.getUTCMonth() === v[1] - 1 && date.getUTCDate() === v[2] && date.getUTCHours() === v[3] && date.getUTCMinutes() === v[4] && date.getUTCSeconds() === v[5]) {
        const quarters = (t[6] & 7) * 10 + (t[6] >> 4), sign = t[6] & 8 ? -1 : 1;
        timestamp = date.getTime() - quarters * sign * 900000;
      }
    } else { const vp = (first >> 3) & 3; if (vp === 2) take(1); else if (vp === 1 || vp === 3) take(7); }
    let coding;
    if (dcs <= 0x3f) { if (dcs & 0x20) throw new Error('暂不支持压缩短信'); coding = (dcs >> 2) & 3; }
    else if ((dcs & 0xc0) === 0xc0 && (dcs & 0xf0) !== 0xf0) coding = (dcs & 0xf0) === 0xe0 ? 2 : 0;
    else if ((dcs & 0xf0) === 0xf0) coding = (dcs >> 2) & 1;
    else throw new Error('此短信编码暂不支持');
    const count = byte(), data = take(coding === 0 ? Math.ceil(count * 7 / 8) : count);
    if (data.length > 140) throw new Error('短信数据过长');
    let skip = 0, concat = null;
    if (first & 0x40) {
      if (!data.length) throw new Error('短信分段头缺失');
      skip = data[0] + 1; if (skip > data.length) throw new Error('短信分段头不完整');
      for (let i = 1; i < skip;) {
        if (i + 2 > skip) throw new Error('短信分段头无效');
        const tag = data[i++], n = data[i++]; if (i + n > skip) throw new Error('短信分段头无效');
        if (tag === 0 && n === 3) concat = { ref: data[i], total: data[i + 1], seq: data[i + 2] };
        if (tag === 8 && n === 4) concat = { ref: data[i] * 256 + data[i + 1], total: data[i + 2], seq: data[i + 3] };
        i += n;
      }
      if (concat && (!concat.seq || concat.seq > concat.total || !concat.total)) throw new Error('短信分段编号无效');
    }
    let body = '';
    if (coding === 0) { const header = Math.ceil(skip * 8 / 7); if (header > count) throw new Error('短信分段长度无效'); body = gsm(data, count - header, header * 7); }
    else if (coding === 2) { if ((data.length - skip) % 2) throw new Error('短信字符不完整'); for (let i = skip; i < data.length; i += 2) body += String.fromCharCode(data[i] * 256 + data[i + 1]); }
    else if (coding === 1) body = '[二进制短信] ' + hex(data.slice(skip));
    else throw new Error('此短信编码暂不支持');
    return { number, body, timestamp, outgoing: kind === 1, concat };
  }
  const codec = { encode, decode };
  if (typeof module !== 'undefined' && module.exports) module.exports = codec;
  else window.QDCSmsCodec = codec;
})();
