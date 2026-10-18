/* Macintosh files on the web page: StuffIt (.sit, .sea) and Compact Pro
 * (.cpt) archives, BinHex (.hqx) and MacBinary (.bin) files are unpacked in
 * the browser and put on an HFS disk image, which is uploaded like any other
 * image. Disk images inside an archive are uploaded as they are instead.
 *
 * The board serves this file as /machine.js; the page loads it when
 * /api/info says "script": true, and it registers itself with
 * registerConverter(). It also runs under node (module.exports) for tests.
 *
 * The decoders follow The Unarchiver's XADMaster and libxad (Dag Ågren and
 * contributors), including XADMaster's code tables for StuffIt method 13,
 * so this file, unlike the rest of the project, is free software under the
 * GNU Lesser General Public License 2.1 or (at your option) any later
 * version: see COPYING.LGPL next to it. The HFS name order is machfs's
 * (Elliot Nunn, MIT).
 */
(function () {
"use strict";

class MacFileError extends Error {}
const fail = msg => { throw new MacFileError(msg); };

const u16 = (b, p) => (b[p] << 8) | b[p + 1];
const u32 = (b, p) => ((b[p] << 24) | (b[p + 1] << 16) | (b[p + 2] << 8) | b[p + 3]) >>> 0;
const put16 = (b, p, v) => { b[p] = v >> 8; b[p + 1] = v; };
const put32 = (b, p, v) => { b[p] = v >>> 24; b[p + 1] = v >>> 16; b[p + 2] = v >>> 8; b[p + 3] = v; };
const fourcc = v => String.fromCharCode(v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const ascii = (b, p, n) => String.fromCharCode(...b.subarray(p, p + n));

/* ---- Checksums ---- */

const CRC16 = new Uint16Array(256);     /* IBM, reflected (StuffIt) */
const CRC32 = new Uint32Array(256);
const CCITT = new Uint16Array(256);     /* XMODEM (BinHex, MacBinary) */
for (let i = 0; i < 256; i++) {
  let a = i, c = i, x = i << 8;
  for (let k = 0; k < 8; k++) {
    a = a & 1 ? (a >>> 1) ^ 0xa001 : a >>> 1;
    c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    x = x & 0x8000 ? (x << 1) ^ 0x1021 : x << 1;
  }
  CRC16[i] = a; CRC32[i] = c >>> 0; CCITT[i] = x;
}
function crc16(b) {
  let c = 0;
  for (let i = 0; i < b.length; i++) c = CRC16[(c ^ b[i]) & 255] ^ (c >>> 8);
  return c;
}
function ccitt(b) {
  let c = 0;
  for (let i = 0; i < b.length; i++) c = (CCITT[(c >> 8) ^ b[i]] ^ (c << 8)) & 0xffff;
  return c;
}

/* ---- Bit streams ---- */

/* Low bit of each byte first; values low bit first (methods 2 and 13) */
function bitsLE(b, p, end) {
  let acc = 0, n = 0;
  return {
    bits(k) {
      while (n < k) { acc |= (p < end ? b[p] : 0) << n; p++; n += 8; }
      const v = acc & ((1 << k) - 1);
      acc >>>= k; n -= k;
      return v;
    },
    skip(k) { while (k > 16) { this.bits(16); k -= 16; } this.bits(k); },
    done() { return p >= end && n === 0; },
  };
}

/* High bit first (methods 3 and 15) */
function bitsBE(b, p, end) {
  let acc = 0, n = 0;
  return {
    bit() {
      if (!n) { acc = p < end ? b[p] : 0; p++; n = 8; }
      return (acc >> --n) & 1;
    },
    bits(k) { let v = 0; while (k--) v = v * 2 + this.bit(); return v; },
  };
}

/* A canonical prefix code from code lengths (the shortest code is zeros),
 * read one bit at a time, the code's high bit first */
function prefixCode(lengths, n) {
  const count = new Array(33).fill(0), first = [], index = [], symbols = [];
  let max = 0;
  for (let i = 0; i < n; i++) {
    const l = lengths[i];
    if (l >= 1 && l <= 32) { count[l]++; if (l > max) max = l; }
  }
  for (let l = 1, code = 0, at = 0; l <= 32; l++) {
    first[l] = code; index[l] = at;
    for (let i = 0; i < n; i++) if (lengths[i] === l) symbols.push(i);
    code = (code + count[l]) * 2; at += count[l];
  }
  return { count, first, index, symbols, max };
}
function readSymbol(code, bit) {
  let v = 0;
  for (let l = 1; l <= code.max; l++) {
    v = v * 2 + bit();
    const d = v - code.first[l];
    if (d >= 0 && d < code.count[l]) return code.symbols[code.index[l] + d];
  }
  fail("damaged archive (bad code)");
}

/* ---- StuffIt method 1: run lengths (0x90) ---- */

function unRLE90(src, out) {
  let o = 0, last = 0;
  for (let i = 0; i < src.length && o < out.length; i++) {
    const b = src[i];
    if (b !== 0x90) { out[o++] = last = b; continue; }
    const n = src[++i];
    if (n === 0) { out[o++] = last = 0x90; continue; }
    for (let k = 1; k < n && o < out.length; k++) out[o++] = last;
  }
  return o;
}

/* ---- Method 2: LZW as in Unix compress (block mode, 14 bits) ---- */

function unLZW(src, out) {
  const MAX = 1 << 14, bits = bitsLE(src, 0, src.length);
  const parent = new Int32Array(MAX), chr = new Uint8Array(MAX), stack = new Uint8Array(MAX);
  for (let i = 0; i < 256; i++) { parent[i] = -1; chr[i] = i; }
  const firstByte = s => { while (parent[s] >= 0) s = parent[s]; return chr[s]; };
  let num = 257, prev = -1, size = 9, counter = 0, o = 0;
  while (o < out.length && !bits.done()) {
    const sym = bits.bits(size);
    counter++;
    if (sym === 256) {
      /* a clear: the rest of its group of eight codes is padding */
      if (counter % 8) bits.skip(size * (8 - counter % 8));
      num = 257; prev = -1; size = 9; counter = 0;
      continue;
    }
    if (prev < 0) {
      if (sym >= num) fail("damaged archive (LZW)");
      prev = sym;
    } else {
      let post;
      if (sym < num) post = firstByte(sym);
      else if (sym === num) post = firstByte(prev);
      else fail("damaged archive (LZW)");
      const par = prev;
      prev = sym;
      if (num < MAX) {
        parent[num] = par; chr[num] = post; num++;
        if (num < MAX && (num & (num - 1)) === 0) size++;
      }
    }
    let n = 0;
    for (let s = prev; s >= 0; s = parent[s]) stack[n++] = chr[s];
    while (n && o < out.length) out[o++] = stack[--n];
  }
  return o;
}

/* ---- Method 3: Huffman with the tree in front ---- */

function unHuffman(src, out) {
  const bits = bitsBE(src, 0, src.length);
  const tree = depth => {
    if (depth > 256) fail("damaged archive (Huffman)");
    if (bits.bit()) return bits.bits(8);
    const zero = tree(depth + 1);
    return [zero, tree(depth + 1)];
  };
  const root = tree(0);
  for (let o = 0; o < out.length; o++) {
    let n = root;
    while (typeof n !== "number") n = n[bits.bit()];
    out[o] = n;
  }
  return out.length;
}

/* ---- Method 5: LZAH (LHarc's adaptive Huffman, 4 KB window) ---- */

const LZAH_DISTANCES = prefixCode([..."3444555555556666666666667777777777777777777777778888888888888888"].map(Number), 64);

function unLZAH(src, out) {
  const bits = bitsBE(src, 0, src.length), bit = () => bits.bit();
  const LEAVES = 314, NODES = 2 * LEAVES - 1;
  const store = [], nodes = [];
  for (let i = 0; i < NODES; i++) store.push(nodes[i] = { index: i, freq: 0, value: 0, left: null, right: null, parent: null });
  for (let i = 0; i < LEAVES; i++) { const n = nodes[NODES - 1 - i]; n.freq = 1; n.value = i; }
  for (let i = LEAVES - 2; i >= 0; i--) {
    const n = nodes[i];
    n.left = nodes[2 * i + 1]; n.right = nodes[2 * i + 2];
    n.left.parent = n.right.parent = n;
    n.freq = n.left.freq + n.right.freq;
  }
  const rebuild = () => {
    const leaves = [];
    for (let i = 0; i < NODES; i++) {
      const n = nodes[i];
      if (!n.left && !n.right) { n.freq = (n.freq + 1) >> 1; leaves.push(n); }
    }
    let leaf = LEAVES - 1, branchAt = LEAVES - 2, at = NODES - 1, pair = NODES - 2;
    while (at >= 0) {
      while (at >= pair) { nodes[at] = leaves[leaf--]; nodes[at].index = at; at--; }
      const b = store[branchAt--];
      b.left = nodes[pair]; b.right = nodes[pair + 1];
      b.left.parent = b.right.parent = b;
      b.freq = b.left.freq + b.right.freq;
      while (leaf >= 0 && leaves[leaf].freq <= b.freq) { nodes[at] = leaves[leaf--]; nodes[at].index = at; at--; }
      nodes[at] = b; b.index = at; at--;
      pair -= 2;
    }
    nodes[0].parent = null;
  };
  const update = n => {
    if (store[0].freq === 0x8000) rebuild();
    for (;;) {
      n.freq++;
      if (!n.parent) break;
      /* keep the nodes ordered by frequency: swap with the first lighter one */
      let q = n.index;
      while (q > 0 && nodes[q - 1].freq < n.freq) q--;
      if (q < n.index) {
        const m = nodes[q], np = n.parent, mp = m.parent;
        const nRight = np.right === n, mRight = mp.right === m;        /* before either changes */
        if (nRight) np.right = m; else np.left = m;
        if (mRight) mp.right = n; else mp.left = n;
        n.parent = mp; m.parent = np;
        nodes[n.index] = m; m.index = n.index;
        nodes[q] = n; n.index = q;
      }
      n = n.parent;
    }
  };
  /* StuffIt starts with a window of spaces (LHarc with runs and ramps) */
  const win = new Uint8Array(4096).fill(0x20);
  let o = 0;
  while (o < out.length) {
    let n = store[0];
    while (n.left || n.right) {
      n = bit() ? n.left : n.right;
      if (!n) fail("damaged archive (LZAH)");
    }
    update(n);
    if (n.value < 0x100) { out[o] = win[o & 4095] = n.value; o++; continue; }
    let length = n.value - 0x100 + 3;
    const offset = (readSymbol(LZAH_DISTANCES, bit) << 6) + bits.bits(6) + 1;
    for (; length > 0 && o < out.length; length--, o++) out[o] = win[o & 4095] = win[(o - offset) & 4095];
  }
  return o;
}

/* ---- Method 13: LZSS + Huffman ---- */

const M13 = {
  firstCodeLengths: [
    "45788999979998999999999:99::9:9959999:999999997998999999999999999899889999999789799779999:9:::9995987598879988557:585899999:99:99:::::::9:::::::9:::::::::::::::9:::::::99::::::::::::::::9:::::9:::::::::::::::::::::::::::::::9:::::::::::99::9:::::::9:::9:956558999999:::9:::::::::::::::::::::9:999:9:9:9:9:::9:9::999699:95",
    "4778788887878798889999::9:::::9959899;:9899989788899999:999:99:988777889889988787:8779999::;;;:9868775777698676679666788889:9:9989::9::99:::::::9::;:::::::;:;::9;::::::99:;:;:;:<:;:<;<:<:;:;;;9:;;;<<:::;;:;::9;:;:;;;:;;<;;:::;::;;<::;;<;;:;9<:;;;:;:;:;9:97356677888999;:::<=;<<;=<<;<<=<>=>=?=>??>=??>?>??>?==>??>>@@???<?:",
    "6666698849898999899:8:::9:::9::9998:9:9:9:9:99898999::::999:9:9978898999899:99898988899999:::::98898978898::89888:88889999:::::9799:::::9::::::99::::::::9::::::9:::::::999:::::::::::::::9::::989::::::::::9:::9::::::::::::::99::::::9::::::999::::::99:998989466678899:::9:::::::::::::::::7:::7::7777767:77:77767667766969:6:",
    "266778787889899988999::98:9:9:9969899:999:9999888889999999999::977888899789:88788:8889899:9;:;998798868887:978778:77789999:;9;:979:::;;::;:::;;:9::;:;:;:::;:;::9::;::::9::::;:;:;:;;;:<:;:;:;;:8::;:;;;:;:;:;;;9:;;:;;;:;;;:::::;::;;::9;::;;:::;::::::9;::8:86566778889:;::;;<<:;<<<<=====<==?><>?@<<=?>@?AB?A@????==:><=AAB:A4",
    "79999999989997999999999:9:9:9:9959799999777977878877999977799999979777799799777779787999999999999787778867977875695756779899999999:9:::99:::::::9:::::::::::9:::9:::99:9999:::::::::::9:::::::::9:::9:::999:::::9:9::9::9:::::::9:::::::::::::::9:::::::9:9:9::9568877799999999999999999999999999:::::::::::::::::::::::9::5:8989"],
  secondCodeLengths: [
    "456677677768788889698989998:5979698:9:8899798989886998899:899:8:8888897:699;78898:7869:99:8;9;9:9898888:99::89888;98899:8;::8:9:899;9;9::;:<9<:;:;9::;:;:;:;:::9998768;;9<:<9;;;:<;;:<:;:::;:;;;9<:<;<:;:<;<;<;<:<;<;;:<:;:<:<:<:;;;:;;;:<;<::;;9<;<:;:<:;:<:;:754667778877686779899:;;;<;:;<;<;<<<<;<<;<;<;=;<:=:>>=>?>@??BBB9B8",
    "566667777778787778888989899979886989898989898988888989899:8:899888788989798:8989898889999:9;9:9:88898889989:89888;8789999:9:9:98899:9:9:8:9:9;:;9;:::;9;9:9;9;::9:998:9;999;:;9;9;9;:;:;:;9::;::8:9::;9;9::;9::99:9:9:9:9;9;::9:9;9;9;9:9;9;9;9:8;9:9:9:8:898987445666777788878899::::::;;::<;;<<;<<;<<<<<<;<;=<=<=>>>?=>=>BBA7@9",
    "56666777687879887789999:899:8:986:8:8:99999:998989899:9:998:9;:88889799:898;9:9:89999899:::<:;::89998988:9:;8:998<8999989:9<:::87;:9:;9;7;:<:<:;9;9<:<:<:9;<:<:;9:9:9;;<9:8<;<9<:<:=:<:<:<:9:<:98;:<:<:<:;:<8<:;:::<9;:<:<;<:9:<9::<:;:;:<8<9<8<8;:;:;9:8:99898743556566778887779899;9;9899;<;<<==<=>=>=>===<==<==>>==>>>>?BAB8@:",
    "4566667767796887788869887989898969898:998:8:8989887999998:9:9:98789989997:9:99898988899:998;9;::88:88999:9:;999989888::998:;:;;989:;9:;;9<:::<;;9;;<9;9::::<9;:;9;;;:;;<9::<;;:;9;:;:;9;;98;:;;:7<;;;;;<:<;=;:<;:;:;:;;;:<;;:;:::;:<;<:;9;:;:;:<9;;;9;::9;::9:974555667687899788:9::<:;;;;:;<;;;;;=<;<=<<<=;9<=7=;=;:;=??<>???6?5",
    "8:;;;<;;<6;<:5<<<<<<<==>==<=<=<?4:79;;:967896767877888888987:9::;786788987::87887:767998;;;:;;;8;676666876:967667:656777:8;9=7>@<>>??@@>????????>?=>>@?A>A?A<>=@<A=A>==>><=??>?A>A?>?@<@?>?@?@AA??AA=>??=<@@A>?@??==?=@A?AAA@A>A>@?A??>A?A?@??@@>AA??@?A?>@@@@@<4455666777888899999:::;:;;;;;<<<==<=<>><====><==>>>=>>??=?=AAA9A7"],
  offsetCodeLengths: [
    "56333333346",
    "5644333334446",
    "67443333344457",
    "36542333446",
    "67764322336"],
  metaCodes: [0x5d8,0x58,0x40,0xc0,0x0,0x78,0x2b,0x14,0xc,0x1c,0x1b,0xb,0x10,0x20,0x38,0x18,0xd8,0xbd8,0x180,0x680,0x380,0xf80,0x780,0x480,0x80,0x280,0x3d8,0xfd8,0x7d8,0x9d8,0x1d8,0x4,0x1,0x2,0x7,0x3,0x8],
  metaCodeLengths: [11,8,8,8,8,7,6,5,5,5,5,6,5,6,7,7,9,12,10,11,11,12,12,11,11,11,12,12,12,12,12,5,2,2,3,4,5],
};
const tableLengths = s => [...s].map(c => c.charCodeAt(0) - 48);

function unSit13(src, out) {
  const bits = bitsLE(src, 1, src.length), bit = () => bits.bits(1);
  const head = src[0], kind = head >> 4;
  let firstCode, secondCode, offsetCode;
  if (kind === 0) {
    /* the codes' lengths, written with a code of their own (low bit first) */
    const meta = new Map();
    M13.metaCodes.forEach((c, i) => meta.set(M13.metaCodeLengths[i] * 65536 + c, i));
    const metaSymbol = () => {
      for (let v = 0, l = 0; l < 16;) {
        v |= bit() << l++;
        const s = meta.get(l * 65536 + v);
        if (s !== undefined) return s;
      }
      fail("damaged archive (code lengths)");
    };
    const readCode = n => {
      const lengths = new Int32Array(n + 80);
      let length = 0;
      for (let i = 0; i < n; i++) {
        const v = metaSymbol();
        if (v === 31) length = -1;
        else if (v === 32) length++;
        else if (v === 33) length--;
        else if (v === 34) { if (bit()) lengths[i++] = length; }
        else if (v === 35) { for (let k = bits.bits(3) + 2; k > 0; k--) lengths[i++] = length; }
        else if (v === 36) { for (let k = bits.bits(6) + 10; k > 0; k--) lengths[i++] = length; }
        else length = v + 1;
        lengths[i] = length;
      }
      return prefixCode(lengths, n);
    };
    firstCode = readCode(321);
    secondCode = head & 8 ? firstCode : readCode(321);
    offsetCode = readCode((head & 7) + 10);
  } else if (kind <= 5) {
    firstCode = prefixCode(tableLengths(M13.firstCodeLengths[kind - 1]), 321);
    secondCode = prefixCode(tableLengths(M13.secondCodeLengths[kind - 1]), 321);
    const offs = tableLengths(M13.offsetCodeLengths[kind - 1]);
    offsetCode = prefixCode(offs, offs.length);
  } else {
    fail("damaged archive (method 13)");
  }
  let code = firstCode, o = 0;
  while (o < out.length) {
    const s = readSymbol(code, bit);
    if (s < 0x100) { out[o++] = s; code = firstCode; continue; }
    code = secondCode;
    let length;
    if (s < 0x13e) length = s - 0x100 + 3;
    else if (s === 0x13e) length = bits.bits(10) + 65;
    else if (s === 0x13f) length = bits.bits(15) + 65;
    else break;
    const b = readSymbol(offsetCode, bit);
    const offset = b === 0 ? 1 : b === 1 ? 2 : (1 << (b - 1)) + bits.bits(b - 1) + 1;
    for (; length > 0 && o < out.length; length--, o++) out[o] = o >= offset ? out[o - offset] : 0;
  }
  return o;
}

/* ---- Method 15 (Arsenic): arithmetic coding, BWT, move-to-front ---- */

const RANDOM = [
  0xee, 0x56, 0xf8, 0xc3, 0x9d, 0x9f, 0xae, 0x2c, 0xad, 0xcd, 0x24, 0x9d, 0xa6, 0x101, 0x18, 0xb9,
  0xa1, 0x82, 0x75, 0xe9, 0x9f, 0x55, 0x66, 0x6a, 0x86, 0x71, 0xdc, 0x84, 0x56, 0x96, 0x56, 0xa1,
  0x84, 0x78, 0xb7, 0x32, 0x6a, 0x03, 0xe3, 0x02, 0x11, 0x101, 0x08, 0x44, 0x83, 0x100, 0x43, 0xe3,
  0x1c, 0xf0, 0x86, 0x6a, 0x6b, 0x0f, 0x03, 0x2d, 0x86, 0x17, 0x7b, 0x10, 0xf6, 0x80, 0x78, 0x7a,
  0xa1, 0xe1, 0xef, 0x8c, 0xf6, 0x87, 0x4b, 0xa7, 0xe2, 0x77, 0xfa, 0xb8, 0x81, 0xee, 0x77, 0xc0,
  0x9d, 0x29, 0x20, 0x27, 0x71, 0x12, 0xe0, 0x6b, 0xd1, 0x7c, 0x0a, 0x89, 0x7d, 0x87, 0xc4, 0x101,
  0xc1, 0x31, 0xaf, 0x38, 0x03, 0x68, 0x1b, 0x76, 0x79, 0x3f, 0xdb, 0xc7, 0x1b, 0x36, 0x7b, 0xe2,
  0x63, 0x81, 0xee, 0x0c, 0x63, 0x8b, 0x78, 0x38, 0x97, 0x9b, 0xd7, 0x8f, 0xdd, 0xf2, 0xa3, 0x77,
  0x8c, 0xc3, 0x39, 0x20, 0xb3, 0x12, 0x11, 0x0e, 0x17, 0x42, 0x80, 0x2c, 0xc4, 0x92, 0x59, 0xc8,
  0xdb, 0x40, 0x76, 0x64, 0xb4, 0x55, 0x1a, 0x9e, 0xfe, 0x5f, 0x06, 0x3c, 0x41, 0xef, 0xd4, 0xaa,
  0x98, 0x29, 0xcd, 0x1f, 0x02, 0xa8, 0x87, 0xd2, 0xa0, 0x93, 0x98, 0xef, 0x0c, 0x43, 0xed, 0x9d,
  0xc2, 0xeb, 0x81, 0xe9, 0x64, 0x23, 0x68, 0x1e, 0x25, 0x57, 0xde, 0x9a, 0xcf, 0x7f, 0xe5, 0xba,
  0x41, 0xea, 0xea, 0x36, 0x1a, 0x28, 0x79, 0x20, 0x5e, 0x18, 0x4e, 0x7c, 0x8e, 0x58, 0x7a, 0xef,
  0x91, 0x02, 0x93, 0xbb, 0x56, 0xa1, 0x49, 0x1b, 0x79, 0x92, 0xf3, 0x58, 0x4f, 0x52, 0x9c, 0x02,
  0x77, 0xaf, 0x2a, 0x8f, 0x49, 0xd0, 0x99, 0x4d, 0x98, 0x101, 0x60, 0x93, 0x100, 0x75, 0x31, 0xce,
  0x49, 0x20, 0x56, 0x57, 0xe2, 0xf5, 0x26, 0x2b, 0x8a, 0xbf, 0xde, 0xd0, 0x83, 0x34, 0xf4, 0x17,
];

function model(first, last, increment, limit) {
  const m = { first, n: last - first + 1, increment, limit, freq: new Int32Array(last - first + 1), total: 0 };
  m.reset = () => { m.freq.fill(increment); m.total = increment * m.n; };
  m.reset();
  return m;
}

function unArsenic(src, out) {
  const bits = bitsBE(src, 0, src.length);
  const ONE = 1 << 25, HALF = 1 << 24;
  let range = ONE, code = bits.bits(26);
  const symbol = m => {
    const f = Math.floor(code / Math.floor(range / m.total));
    let cum = 0, n = 0;
    for (; n < m.n - 1; n++) {
      if (cum + m.freq[n] > f) break;
      cum += m.freq[n];
    }
    const size = m.freq[n], factor = Math.floor(range / m.total), low = factor * cum;
    code -= low;
    if (cum + size === m.total) range -= low;
    else range = size * factor;
    while (range <= HALF) { range *= 2; code = code * 2 + bits.bit(); }
    m.freq[n] += m.increment;
    m.total += m.increment;
    if (m.total > m.limit) {
      m.total = 0;
      for (let i = 0; i < m.n; i++) { m.freq[i] = (m.freq[i] + 1) >> 1; m.total += m.freq[i]; }
    }
    return m.first + n;
  };
  const bitString = (m, k) => { let v = 0; for (let i = 0; i < k; i++) if (symbol(m)) v |= 1 << i; return v >>> 0; };

  const initial = model(0, 1, 1, 256), selector = model(0, 10, 8, 1024);
  const mtfModels = [model(2, 3, 8, 1024), model(4, 7, 4, 1024), model(8, 15, 4, 1024),
    model(16, 31, 4, 1024), model(32, 63, 2, 1024), model(64, 127, 2, 1024), model(128, 255, 1, 1024)];
  if (bitString(initial, 8) !== 0x41 || bitString(initial, 8) !== 0x73) fail("damaged archive (Arsenic)");
  const blockBits = bitString(initial, 4) + 9, blockSize = 1 << blockBits;
  const block = new Uint8Array(blockSize), transform = new Uint32Array(blockSize), mtf = new Uint8Array(256);
  let end = symbol(initial), crc = 0xffffffff, stored = 0, o = 0;
  let repeat = 0, count = 0, last = 0;

  while (o < out.length && !end) {
    /* one block: move-to-front symbols with runs of zeros */
    for (let i = 0; i < 256; i++) mtf[i] = i;
    const toFront = s => { const v = mtf[s]; mtf.copyWithin(1, 0, s); mtf[0] = v; return v; };
    const randomized = symbol(initial);
    let index = bitString(initial, blockBits), size = 0;
    for (;;) {
      let sel = symbol(selector);
      if (sel < 2) {
        let state = 1, zeros = 0;
        while (sel < 2) {
          zeros += sel === 0 ? state : 2 * state;
          state *= 2;
          sel = symbol(selector);
        }
        if (size + zeros > blockSize) fail("damaged archive (Arsenic)");
        block.fill(toFront(0), size, size + zeros);
        size += zeros;
      }
      if (sel === 10) break;
      const s = sel === 2 ? 1 : symbol(mtfModels[sel - 3]);
      if (size >= blockSize) fail("damaged archive (Arsenic)");
      block[size++] = toFront(s);
    }
    if (index >= size) fail("damaged archive (Arsenic)");
    selector.reset();
    mtfModels.forEach(m => m.reset());
    if (symbol(initial)) { stored = bitString(initial, 32); end = true; }

    /* undo the BWT, then the randomisation and the run lengths */
    const counts = new Int32Array(256), cumulative = new Int32Array(256);
    for (let i = 0; i < size; i++) counts[block[i]]++;
    for (let i = 0, t = 0; i < 256; i++) { cumulative[i] = t; t += counts[i]; counts[i] = 0; }
    for (let i = 0; i < size; i++) transform[cumulative[block[i]] + counts[block[i]]++] = i;
    let randIndex = 0, randCount = RANDOM[0];
    count = 0; last = 0;
    for (let n = 0; n < size && o < out.length; n++) {
      index = transform[index];
      let b = block[index];
      if (randomized && randCount === n) {
        b ^= 1;
        randIndex = (randIndex + 1) & 255;
        randCount += RANDOM[randIndex];
      }
      if (count === 4) {
        count = 0;
        for (repeat = b; repeat > 0 && o < out.length; repeat--) {
          out[o++] = last;
          crc = CRC32[(crc ^ last) & 255] ^ (crc >>> 8);
        }
        continue;
      }
      if (b === last) count++;
      else { count = 1; last = b; }
      out[o++] = b;
      crc = CRC32[(crc ^ b) & 255] ^ (crc >>> 8);
    }
  }
  if (o === out.length && end && (stored >>> 0) !== (~crc >>> 0)) fail("damaged archive (checksum)");
  return o;
}

/* A fork's data: method, compressed bytes, length, CRC-16 (0 = none) */
const methodsSeen = {};                    /* compression methods met (for tests) */

function unpack(method, src, length, crc, what) {
  const out = new Uint8Array(length);
  if (!length) return out;
  methodsSeen[method & 15] = (methodsSeen[method & 15] || 0) + 1;
  let n;
  switch (method & 15) {
    case 0: out.set(src.subarray(0, length)); n = Math.min(src.length, length); break;
    case 1: n = unRLE90(src, out); break;
    case 2: n = unLZW(src, out); break;
    case 3: n = unHuffman(src, out); break;
    case 5: n = unLZAH(src, out); break;
    case 13: n = unSit13(src, out); break;
    case 15: return unArsenic(src, out) === length ? out : fail(what + ": damaged (too short)");
    default: fail(what + ": compressed with StuffIt method " + (method & 15) + ", which this page can't unpack");
  }
  if (n !== length) fail(what + ": damaged (too short)");
  if (crc16(out) !== crc) fail(what + ": damaged (checksum)");
  return out;
}

/* ---- StuffIt archives -> a tree of {name, dir, children} and
 *      {name, type, creator, flags, created, modified, data, rsrc} ---- */

const SIT5_TEXT = "StuffIt (c)1997-";

function sitOffset(b) {
  const limit = Math.min(b.length - 100, 1 << 16);
  for (let i = 0; i <= limit; i++) {
    if (b[i] === 0x53 && ascii(b, i, 16) === SIT5_TEXT) return { at: i, v5: true };
    if (u32(b, i + 10) === 0x724c6175 && /^(SIT!|ST46|ST50|ST60|ST65|STin|STi\d|ST\d\d)$/.test(ascii(b, i, 4)))
      return { at: i, v5: false };
  }
  return null;
}

function parseSit(b, found, label) {
  b = b.subarray(found.at);
  const root = [];
  const check = (p, n) => { if (p + n > b.length) fail(label + ": damaged or incomplete archive"); };
  if (!found.v5) {
    /* StuffIt 1.5 to 4.5 */
    const total = Math.min(u32(b, 6), b.length);
    const stack = [root];
    for (let p = 22; p + 112 <= total;) {
      const h = b.subarray(p, p + 112);
      if (crc16(h.subarray(0, 110)) !== u16(h, 110)) fail(label + ": damaged archive (header checksum)");
      p += 112;
      const rm = h[0], dm = h[1], name = h.slice(3, 3 + Math.min(h[2], 31));
      const folder = (rm & 0x6f) === 0x20 || (dm & 0x6f) === 0x20;
      const folderEnd = (rm & 0x6f) === 0x21 || (dm & 0x6f) === 0x21;
      if (folder) {
        const dir = { name, dir: true, flags: u16(h, 74), created: u32(h, 76), modified: u32(h, 80), children: [] };
        stack[stack.length - 1].push(dir);
        stack.push(dir.children);
      } else if (folderEnd) {
        if (stack.length > 1) stack.pop();
      } else {
        if ((rm | dm) & 0x80) fail(label + ": the archive has a password");
        const rlen = u32(h, 84), dlen = u32(h, 88), rcomp = u32(h, 92), dcomp = u32(h, 96);
        check(p, rcomp + dcomp);
        const what = label + ": " + macToString(name);
        stack[stack.length - 1].push({
          name, type: u32(h, 66), creator: u32(h, 70), flags: u16(h, 74),
          created: u32(h, 76), modified: u32(h, 80),
          rsrc: unpack(rm, b.subarray(p, p + rcomp), rlen, u16(h, 100), what),
          data: unpack(dm, b.subarray(p + rcomp, p + rcomp + dcomp), dlen, u16(h, 102), what),
        });
        p += rcomp + dcomp;
      }
    }
    return root;
  }

  /* StuffIt 5 and later */
  if (b[82] !== 5) fail(label + ": StuffIt archive version " + b[82] + " (this page reads 1.5 to 5)");
  if (b[83] & 0x80) fail(label + ": the archive has a password");
  let entries = u16(b, 92), p = u32(b, 94);
  const dirs = new Map();
  for (let i = 0; i < entries; i++) {
    check(p, 48);
    const at = p;
    if (u32(b, p) !== 0xa5a5a5a5) fail(label + ": damaged archive (entry)");
    const version = b[p + 4], headerSize = u16(b, p + 6), flags = b[p + 9];
    const created = u32(b, p + 10), modified = u32(b, p + 14), parentAt = u32(b, p + 26);
    const nameLength = u16(b, p + 30);
    const dlen = u32(b, p + 34), dcomp = u32(b, p + 38), dcrc = u16(b, p + 42);
    const isDir = (flags & 0x40) !== 0;
    let q = p + 46, dm = 0, children = 0;
    if (isDir) {
      children = u16(b, q); q += 2;
      if (dlen === 0xffffffff) { entries++; p = q; continue; }   /* a marker after folders */
    } else {
      dm = b[q];
      if ((flags & 0x20) && dlen) fail(label + ": the archive has a password");
      q += 2 + b[q + 1];
    }
    const name = b.slice(q, q + nameLength);
    q += nameLength;
    if (q < at + headerSize) q += 4 + u16(b, q);                  /* comment */
    check(q, 14);
    const hasRsrc = u16(b, q) & 1;
    const type = u32(b, q + 4), creator = u32(b, q + 8), finder = u16(b, q + 12);
    q += 14 + (version === 1 ? 22 : 18);
    let rlen = 0, rcomp = 0, rcrc = 0, rm = 0;
    if (hasRsrc) {
      check(q, 14);
      rlen = u32(b, q); rcomp = u32(b, q + 4); rcrc = u16(b, q + 8); rm = b[q + 12];
      if ((flags & 0x20) && rlen) fail(label + ": the archive has a password");
      q += 14 + b[q + 13];
    }
    const parent = dirs.get(parentAt) || root;
    if (isDir) {
      const dir = { name, dir: true, flags: finder, created, modified, children: [] };
      dirs.set(at, dir.children);
      parent.push(dir);
      entries += children;
      p = q;
    } else {
      check(q, rcomp + dcomp);
      const what = label + ": " + macToString(name);
      parent.push({
        name, type, creator, flags: finder, created, modified,
        rsrc: unpack(rm, b.subarray(q, q + rcomp), rlen, rcrc, what),
        data: unpack(dm, b.subarray(q + rcomp, q + rcomp + dcomp), dlen, dcrc, what),
      });
      p = q + rcomp + dcomp;
    }
  }
  return root;
}

/* ---- Compact Pro ---- */

/* Compact Pro's run lengths: 0x81 0x82 n repeats, 0x81 0x82 0 is 0x81 0x82 */
function unCptRLE(src, out) {
  let i = 0, o = 0, saved = 0, repeat = 0, half = false;
  const next = () => (i < src.length ? src[i++] : 0);
  while (o < out.length) {
    if (repeat) { repeat--; out[o++] = saved; continue; }
    let b = half ? 0x81 : next();
    half = false;
    if (b !== 0x81) { out[o++] = saved = b; continue; }
    b = next();
    if (b === 0x82) {
      b = next();
      if (b) { repeat = b - 2; out[o++] = saved; }
      else { repeat = 1; saved = 0x82; out[o++] = 0x81; }
    } else if (b === 0x81) {
      half = true; out[o++] = saved = 0x81;
    } else {
      repeat = 1; saved = b; out[o++] = 0x81;
    }
  }
  return i;
}

/* Compact Pro's LZH: blocks of Huffman-coded literals and matches, 8 KB window */
function unCptLZH(src) {
  let p = 0, acc = 0, n = 0;
  const bit = () => {
    if (!n) { acc = p < src.length ? src[p] : 0; p++; n = 8; }
    return (acc >> --n) & 1;
  };
  const bits = k => { let v = 0; while (k--) v = v * 2 + bit(); return v; };
  const byte = () => { n = 0; return p < src.length ? src[p++] : 0; };
  const table = size => {
    const count = byte(), lengths = new Int32Array(size);
    if (count * 2 > size) fail("damaged archive (Compact Pro)");
    for (let i = 0; i < count; i++) { const v = byte(); lengths[2 * i] = v >> 4; lengths[2 * i + 1] = v & 15; }
    return prefixCode(lengths, size);
  };
  let out = new Uint8Array(Math.max(1024, src.length * 3)), o = 0;
  const grow = need => { if (o + need > out.length) { const t = new Uint8Array((o + need) * 2); t.set(out.subarray(0, o)); out = t; } };
  let literals, lengths, offsets, count = 0x1fff0, start = 0;
  while (p < src.length) {
    if (count >= 0x1fff0) {
      if (start) { n = 0; p += (p - start) & 1 ? 3 : 2; }    /* block padding */
      if (p >= src.length) break;
      literals = table(256); lengths = table(64); offsets = table(128);
      count = 0; start = p;
    }
    if (bit()) {
      count += 2;
      grow(1);
      out[o++] = readSymbol(literals, bit);
    } else {
      count += 3;
      const length = readSymbol(lengths, bit);
      /* the 8 KB window wraps: offset 0 is 8192 bytes back */
      const offset = ((readSymbol(offsets, bit) << 6) | bits(6)) || 8192;
      grow(length);
      for (let k = 0; k < length; k++, o++) out[o] = o >= offset ? out[o - offset] : 0;
    }
  }
  return out.subarray(0, o);
}

function cptDirectory(b) {
  if (b.length < 8 || b[0] !== 1) return -1;
  const at = u32(b, 4);
  if (at + 7 > b.length) return -1;
  let p = at + 4, crc = 0xffffffff;
  const add = n => { if (p + n > b.length) throw 0; for (let k = 0; k < n; k++) crc = CRC32[(crc ^ b[p++]) & 255] ^ (crc >>> 8); };
  try {
    const entries = u16(b, p);
    add(2); add(b[p] + 1);
    for (let i = 0; i < entries; i++) { const l = b[p]; add(1); add(l & 0x7f); add(l & 0x80 ? 2 : 45); }
  } catch (e) {
    return -1;
  }
  return (crc >>> 0) === u32(b, at) ? at : -1;
}

function parseCpt(b, at, label) {
  let p = at + 6;
  p += 1 + b[p];                                                 /* comment */
  const fork = (src, length, lzh) => {
    const out = new Uint8Array(length);
    if (length) unCptRLE(lzh ? unCptLZH(src) : src, out);
    return out;
  };
  const list = count => {
    const items = [];
    while (count > 0) {
      const l = b[p++], name = b.slice(p, p + (l & 0x7f));
      p += l & 0x7f;
      if (l & 0x80) {
        const inside = u16(b, p);
        p += 2;
        items.push({ name, dir: true, flags: 0, children: list(inside) });
        count -= inside + 1;
        continue;
      }
      const e = p;
      p += 45;
      const offset = u32(b, e + 1), flags = u16(b, e + 27);
      const rlen = u32(b, e + 29), dlen = u32(b, e + 33), rcomp = u32(b, e + 37), dcomp = u32(b, e + 41);
      const what = label + ": " + macToString(name);
      if (flags & 1) fail(label + ": the archive has a password");
      if (offset + rcomp + dcomp > b.length) fail(label + ": damaged or incomplete archive");
      const rsrc = fork(b.subarray(offset, offset + rcomp), rlen, flags & 2);
      const data = fork(b.subarray(offset + rcomp, offset + rcomp + dcomp), dlen, flags & 4);
      let crc = 0xffffffff;
      for (const f of [rsrc, data]) for (let k = 0; k < f.length; k++) crc = CRC32[(crc ^ f[k]) & 255] ^ (crc >>> 8);
      if ((crc >>> 0) !== u32(b, e + 23)) fail(what + ": damaged (checksum)");
      items.push({
        name, type: u32(b, e + 5), creator: u32(b, e + 9), flags: u16(b, e + 21),
        created: u32(b, e + 13), modified: u32(b, e + 17), rsrc, data,
      });
      count--;
    }
    return items;
  };
  return list(u16(b, at + 4));
}

/* ---- BinHex 4.0 and MacBinary: one Mac file each ---- */

const BINHEX = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

function parseBinHex(b, label) {
  const text = new TextDecoder("latin1").decode(b);
  const mark = text.indexOf("(This file must be converted with BinHex");
  let i = text.indexOf(":", mark < 0 ? 0 : mark);
  if (i < 0) return null;
  const value = new Int8Array(256).fill(-1);
  for (let k = 0; k < 64; k++) value[BINHEX.charCodeAt(k)] = k;
  const raw = new Uint8Array(Math.ceil(text.length * 3 / 4));
  let n = 0, acc = 0, have = 0;
  for (i++; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c === 0x3a) break;
    if (c <= 0x20) continue;
    if (value[c] < 0) fail(label + ": damaged BinHex text");
    acc = (acc << 6) | value[c]; have += 6;
    if (have >= 8) { have -= 8; raw[n++] = (acc >> have) & 255; }
  }
  /* run lengths: 0x90 n repeats the previous byte */
  const out = [];
  for (let k = 0, last = 0; k < n; k++) {
    if (raw[k] !== 0x90) { out.push(last = raw[k]); continue; }
    const count = raw[++k];
    if (count === 0) out.push(last = 0x90);
    else for (let r = 1; r < count; r++) out.push(last);
  }
  const d = Uint8Array.from(out);
  const nl = d[0], h = 1 + nl + 1;
  if (d.length < h + 20) fail(label + ": damaged BinHex file");
  const dlen = u32(d, h + 10), rlen = u32(d, h + 14);
  const data = h + 20, rsrc = data + dlen + 2;
  if (rsrc + rlen > d.length) fail(label + ": incomplete BinHex file");
  if (ccitt(d.subarray(0, h + 18)) !== u16(d, h + 18)) fail(label + ": damaged BinHex file (checksum)");
  return {
    name: d.slice(1, 1 + nl), type: u32(d, h), creator: u32(d, h + 4), flags: u16(d, h + 8),
    created: 0, modified: 0,
    data: d.slice(data, data + dlen), rsrc: d.slice(rsrc, rsrc + rlen),
  };
}

function parseMacBinary(b) {
  if (b.length < 128 || b[0] !== 0 || b[74] !== 0 || b[82] !== 0 || b[1] < 1 || b[1] > 63) return null;
  const dlen = u32(b, 83), rlen = u32(b, 87);
  const crcOk = ccitt(b.subarray(0, 124)) === u16(b, 124);
  if (!crcOk && (u16(b, 124) !== 0 || b[122] !== 0)) return null;     /* MacBinary I has no CRC */
  const data = 128 + ((u16(b, 120) + 127) & ~127), rsrc = data + ((dlen + 127) & ~127);
  if (dlen > b.length || rlen > b.length || rsrc + rlen > b.length) return null;
  return {
    name: b.slice(2, 2 + b[1]), type: u32(b, 65), creator: u32(b, 69),
    flags: (b[73] << 8) | (crcOk ? b[101] : 0), created: u32(b, 91), modified: u32(b, 95),
    data: b.slice(data, data + dlen), rsrc: b.slice(rsrc, rsrc + rlen),
  };
}

/* ---- Names: Mac OS Roman ---- */

const ROMAN = "ÄÅÇÉÑÖÜáàâäãåçéèêëíìîïñóòôöõúùûü†°¢£§•¶ß®©™´¨≠ÆØ∞±≤≥¥µ∂∑∏π∫ªºΩæø" +
  "¿¡¬√ƒ≈∆«»… ÀÃÕŒœ–—“”‘’÷◊ÿŸ⁄€‹›ﬁﬂ‡·‚„‰ÂÊÁËÈÍÎÏÌÓÔÒÚÛÙıˆ˜¯˘˙˚¸˝˛ˇ";
const FROM_UNICODE = new Map([...ROMAN].map((c, i) => [c, 0x80 + i]));

function macToString(b) {
  let s = "";
  for (const c of b) s += c < 0x80 ? String.fromCharCode(c) : ROMAN[c - 0x80];
  return s;
}
function stringToMac(s) {
  const out = [];
  for (const c of s.normalize("NFC")) {
    const u = c.codePointAt(0);
    out.push(u < 0x80 ? u : FROM_UNICODE.get(c) ?? 0x3f);
  }
  return Uint8Array.from(out);
}
/* Newer StuffIt versions write names in UTF-8 */
function macName(b) {
  if (!b.some(c => c >= 0x80)) return b;
  try {
    return stringToMac(new TextDecoder("utf-8", { fatal: true }).decode(b));
  } catch (e) {
    return b;
  }
}

/* ---- HFS ---- */

const ORDER = [
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x20, 0x22, 0x23, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
  0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
  0x47, 0x48, 0x58, 0x5a, 0x5e, 0x60, 0x67, 0x69, 0x6b, 0x6d, 0x73, 0x75, 0x77, 0x79, 0x7b, 0x7f,
  0x8d, 0x8f, 0x91, 0x93, 0x96, 0x98, 0x9f, 0xa1, 0xa3, 0xa5, 0xa8, 0xaa, 0xab, 0xac, 0xad, 0xae,
  0x54, 0x48, 0x58, 0x5a, 0x5e, 0x60, 0x67, 0x69, 0x6b, 0x6d, 0x73, 0x75, 0x77, 0x79, 0x7b, 0x7f,
  0x8d, 0x8f, 0x91, 0x93, 0x96, 0x98, 0x9f, 0xa1, 0xa3, 0xa5, 0xa8, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3,
  0x4c, 0x50, 0x5c, 0x62, 0x7d, 0x81, 0x9a, 0x55, 0x4a, 0x56, 0x4c, 0x4e, 0x50, 0x5c, 0x62, 0x64,
  0x65, 0x66, 0x6f, 0x70, 0x71, 0x72, 0x7d, 0x89, 0x8a, 0x8b, 0x81, 0x83, 0x9c, 0x9d, 0x9e, 0x9a,
  0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0x95, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0x52, 0x85,
  0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0x57, 0x8c, 0xcc, 0x52, 0x85,
  0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0x26, 0x27, 0xd4, 0x20, 0x4a, 0x4e, 0x83, 0x87, 0x87,
  0xd5, 0xd6, 0x24, 0x25, 0x2d, 0x2e, 0xd7, 0xd8, 0xa7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
  0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
  0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
];
const sortName = b => Array.from(b, c => ORDER[c]);
function compareNames(a, b) {
  const x = sortName(a), y = sortName(b);
  for (let i = 0; i < Math.min(x.length, y.length); i++) if (x[i] !== y[i]) return x[i] - y[i];
  return x.length - y.length;
}

const NODE = 512;

/* A B-tree file of `nodes` nodes holding the sorted leaf records
 * ({key, data}; key starts with its length byte) */
function bTree(records, nodes, maxKeyLength, indexKey) {
  const file = new Uint8Array(nodes * NODE);
  let next = 1;
  const pack = (recs, type, height) => {
    const made = [];
    let cur = [], used = 14 + 2;
    const flush = () => { made.push({ recs: cur, n: next++ }); cur = []; used = 16; };
    for (const r of recs) {
      if (used + r.length + 2 > NODE) flush();
      cur.push(r); used += r.length + 2;
    }
    if (cur.length) flush();
    made.forEach((m, i) => {
      const o = m.n * NODE;
      if (o + NODE > file.length) fail("the files don't fit the catalog");
      /* the nodes of each level are linked, index nodes too */
      if (i + 1 < made.length) put32(file, o, made[i + 1].n);
      if (i > 0) put32(file, o + 4, made[i - 1].n);
      file[o + 8] = type; file[o + 9] = height; put16(file, o + 10, m.recs.length);
      let at = 14;
      m.recs.forEach((r, k) => { put16(file, o + NODE - 2 * (k + 1), at); file.set(r, o + at); at += r.length; });
      put16(file, o + NODE - 2 * (m.recs.length + 1), at);
    });
    return made;
  };
  const even = b => (b.length & 1 ? Uint8Array.from([...b, 0]) : b);
  const leaves = pack(records.map(r => {
    const k = even(r.key), t = new Uint8Array(k.length + r.data.length);
    t.set(k); t.set(r.data, k.length);
    return t;
  }), 0xff, 1);
  /* the first key of every node goes into the level above */
  let at = 0;
  const firsts = leaves.map(m => { const k = records[at].key; at += m.recs.length; return k; });
  let depth = leaves.length ? 1 : 0, items = leaves.map((m, i) => ({ n: m.n, key: firsts[i] }));
  while (items.length > 1) {
    depth++;
    const recs = items.map(it => { const k = indexKey(it.key), t = new Uint8Array(k.length + 4); t.set(k); put32(t, k.length, it.n); return t; });
    const made = pack(recs, 0, depth);
    let pos = 0;
    items = made.map(m => { const it = { n: m.n, key: items[pos].key }; pos += m.recs.length; return it; });
  }
  /* the header node: header record, user data, node map */
  file[8] = 1; put16(file, 10, 3);
  put16(file, 14, depth); put32(file, 16, items.length ? items[0].n : 0);
  put32(file, 20, records.length);
  put32(file, 24, leaves.length ? leaves[0].n : 0); put32(file, 28, leaves.length ? leaves[leaves.length - 1].n : 0);
  put16(file, 32, NODE); put16(file, 34, maxKeyLength);
  put32(file, 36, nodes); put32(file, 40, nodes - next);
  for (let i = 0; i < next; i++) file[248 + (i >> 3)] |= 0x80 >> (i & 7);
  put16(file, NODE - 2, 14); put16(file, NODE - 4, 120); put16(file, NODE - 6, 248); put16(file, NODE - 8, NODE - 8);
  return { file, used: next };
}

/* Catalog keys: length, reserved, parent ID, name */
function catKey(parent, name) {
  const k = new Uint8Array(7 + name.length);
  k[0] = 6 + name.length; put32(k, 2, parent); k[6] = name.length; k.set(name, 7);
  return k;
}
function catIndexKey(key) {
  const k = new Uint8Array(38);
  k.set(key.subarray(0, Math.min(key.length, 38)));
  k[0] = 37;
  return k;
}

function macNow() {
  const now = new Date();
  return Math.floor(now.getTime() / 1000) - now.getTimezoneOffset() * 60 + 2082844800;
}

function allocationBlockSize(bytes) {
  let size = 512;
  while (bytes - 6 * 512 > size * 65536) size += 512;
  return size;
}

/* An HFS volume with the tree's files: the smallest floppy that holds them
 * and `spare` more bytes, or a hard disk with some room to spare */
function buildHfs(volumeName, tree, spare = 0, now = macNow()) {
  /* IDs, names and records */
  let nextId = 16, files = 0, dirs = 0, rootFiles = 0, rootDirs = 0;
  const records = [], forks = [];
  const vname = volumeName.subarray(0, 27);
  const add = (parent, name, data) => records.push({ parent, name, key: catKey(parent, name), data });
  const thread = (id, parent, name) => {
    const t = new Uint8Array(46);
    t[0] = 3; put32(t, 10, parent); t[14] = name.length; t.set(name, 15);
    add(id, new Uint8Array(0), t);
  };
  const dirRecord = (id, count, node) => {
    const d = new Uint8Array(70);
    d[0] = 1; put16(d, 4, count); put32(d, 6, id);
    put32(d, 10, node.created || now); put32(d, 14, node.modified || now);
    put16(d, 30, (node.flags || 0) & ~0x0101);
    return d;
  };
  const unique = (list) => {
    const seen = [];
    return list.map(node => {
      let name = macName(node.name).map(c => (c === 0x3a ? 0x2d : c)).subarray(0, 31);
      if (!name.length) name = Uint8Array.of(0x3f);
      for (let n = 2; seen.some(s => compareNames(s, name) === 0); n++) {
        const tail = stringToMac(" " + n);
        name = Uint8Array.from([...name.subarray(0, 31 - tail.length), ...tail]);
      }
      seen.push(name);
      return { node, name };
    });
  };
  const walk = (list, parent) => {
    for (const { node, name } of unique(list)) {
      if (node.dir) {
        const id = nextId++;
        dirs++;
        if (parent === 2) rootDirs++;
        add(parent, name, dirRecord(id, node.children.length, node));
        thread(id, parent, name);
        walk(node.children, id);
      } else {
        const f = new Uint8Array(102), id = nextId++;
        files++;
        if (parent === 2) rootFiles++;
        f[0] = 2;
        put32(f, 4, node.type); put32(f, 8, node.creator); put16(f, 12, (node.flags || 0) & ~0x0101);
        put32(f, 20, id);
        put32(f, 44, node.created || now); put32(f, 48, node.modified || now);
        forks.push({ record: f, at: 26, ext: 74, bytes: node.data });
        forks.push({ record: f, at: 36, ext: 86, bytes: node.rsrc });
        add(parent, name, f);
      }
    }
  };
  add(1, vname, dirRecord(2, tree.length, { created: now, modified: now }));
  thread(2, 1, vname);
  walk(tree, 2);
  records.sort((a, b) => a.parent - b.parent || compareNames(a.name, b.name));

  /* sizes: try 800K and 1.4M floppies, then hard disks */
  const forkBytes = forks.reduce((t, f) => t + f.bytes.length, 0);
  const catalogUsed = Math.ceil(records.reduce((t, r) => t + r.key.length + 1 + r.data.length + 2, 0) / (NODE - 16) * 1.3) + 4;
  const candidates = [819200, 1474560];
  let size, block, extNodes, catNodes;
  for (let i = 0; ; i++) {
    size = i < candidates.length ? candidates[i]
      : Math.ceil(((forkBytes + spare) * 1.25 + forks.length * 1024 + 512 * 1024) / 1048576) * 1048576 * (i - 1);
    block = allocationBlockSize(size);
    const perBlock = block / NODE;
    extNodes = Math.ceil(Math.min(256, Math.max(4, size / (256 * 1024))) / perBlock) * perBlock;
    catNodes = Math.ceil(Math.max(catalogUsed + 8, extNodes * 2) / perBlock) * perBlock;
    if (catNodes > 2048) fail("too many files for one disk");
    const allocStart = 3 + Math.ceil(size / block / 4096);
    const blocks = Math.floor((size / 512 - allocStart - 2) * 512 / block);
    const need = (extNodes + catNodes) * NODE / block +
      forks.reduce((t, f) => t + Math.ceil(f.bytes.length / block), 0);
    if (need + spare / block <= blocks && (i < candidates.length || need <= blocks * 0.8)) break;
  }
  const img = new Uint8Array(size);
  const allocStart = 3 + Math.ceil(size / block / 4096);
  const totalBlocks = Math.floor((size / 512 - allocStart - 2) * 512 / block);
  let nextBlock = 0;
  const allocate = bytes => {
    const n = Math.ceil(bytes.length / block), start = nextBlock;
    if (n) img.set(bytes, (allocStart * 512) + start * block);
    nextBlock += n;
    return { start: n ? start : 0, count: n };
  };

  /* the trees, then the files' forks; the catalog is written last, when
   * the file records know where their forks are */
  const ext = allocate(bTree([], extNodes, 7, k => k).file);
  const cat = allocate(new Uint8Array(catNodes * NODE));
  for (const f of forks) {
    const a = allocate(f.bytes);
    put32(f.record, f.at, f.bytes.length);
    put32(f.record, f.at + 4, a.count * block);
    put16(f.record, f.ext, a.start); put16(f.record, f.ext + 2, a.count);
  }
  img.set(bTree(records, catNodes, 37, catIndexKey).file, allocStart * 512 + cat.start * block);

  /* volume bitmap and master directory block */
  for (let i = 0; i < nextBlock; i++) img[3 * 512 + (i >> 3)] |= 0x80 >> (i & 7);
  const m = 1024;
  put16(img, m, 0x4244);
  put32(img, m + 2, now); put32(img, m + 6, now);
  put16(img, m + 10, 0x0100);
  put16(img, m + 12, rootFiles);
  put16(img, m + 14, 3);
  put16(img, m + 16, nextBlock);
  put16(img, m + 18, totalBlocks);
  put32(img, m + 20, block);
  put32(img, m + 24, block * 4);
  put16(img, m + 28, allocStart);
  put32(img, m + 30, nextId);
  put16(img, m + 34, totalBlocks - nextBlock);
  img[m + 36] = vname.length; img.set(vname, m + 37);
  put32(img, m + 74, extNodes * NODE);
  put32(img, m + 78, catNodes * NODE);
  put16(img, m + 82, rootDirs);
  put32(img, m + 84, files);
  put32(img, m + 88, dirs);
  put32(img, m + 130, extNodes * NODE);
  put16(img, m + 134, ext.start); put16(img, m + 136, ext.count);
  put32(img, m + 146, catNodes * NODE);
  put16(img, m + 150, cat.start); put16(img, m + 152, cat.count);
  img.copyWithin(size - 1024, m, m + 512);                        /* the spare copy */
  return img;
}

/* ---- What the page calls ---- */

const DC42_SIZES = new Set([409600, 819200, 737280, 1474560]);

/* A disk image inside an archive: its name for the board, or null */
function imageName(file, isImage) {
  const name = macToString(macName(file.name)).replace(/[\\/:*?"<>|\r\n]/g, "_").replace(/^\.+/, "");
  if (!file.data.length) return null;
  if (isImage(name)) return name;
  const d = file.data;
  if (d.length > 84 && d[82] === 1 && d[83] === 0 && 84 + u32(d, 64) + u32(d, 68) === d.length) return name + ".image";
  if (DC42_SIZES.has(d.length) && /^(dImg|rohd)$/.test(fourcc(file.type))) return name + ".dsk";
  return null;
}

/* The archive or file -> a list of Mac files and folders */
function archive(bytes, label) {
  const found = sitOffset(bytes);
  if (found) return parseSit(bytes, found, label);
  const cpt = cptDirectory(bytes);
  return cpt >= 0 ? parseCpt(bytes, cpt, label) : null;
}

function open(bytes, label) {
  const tree = archive(bytes, label);
  if (tree) return tree;
  const text = new TextDecoder("latin1").decode(bytes.subarray(0, 4096));
  const file = /BinHex 4\.0|^\s*:/.test(text) ? parseBinHex(bytes, label) : parseMacBinary(bytes);
  if (!file) fail(label + ": not a StuffIt, Compact Pro, BinHex or MacBinary file");
  /* an archive or self-extracting archive (in the data fork), or one file */
  return archive(file.data, label) || [file];
}

/* name, bytes -> [{name, data}] for the board */
function convert(name, bytes, opts = {}) {
  const isImage = opts.isImage || (() => false), log = opts.log || (() => {});
  const tree = open(bytes, name);
  const all = [];
  const collect = list => list.forEach(n => (n.dir ? collect(n.children) : all.push(n)));
  collect(tree);
  if (!all.length) fail(name + ": the archive is empty");
  const images = all.map(f => ({ f, name: imageName(f, isImage) })).filter(x => x.name);
  if (images.length) {
    if (images.length < all.length) log(name + ": " + (all.length - images.length) + " other file(s) left out; uploading the disk images");
    return images.map(x => ({ name: x.name, data: x.f.data }));
  }
  /* archives this page can't open (self-extracting ones run on the Mac):
   * room to unpack them next to them */
  const packed = all.filter(f => /^(SIT!|SITD|SIT5|PACT|ZIP |DDAR|DDAP)$/.test(fourcc(f.type)) ||
    /\.(sea|sit|cpt|zip|dd)$/i.test(macToString(macName(f.name))));
  const spare = 3 * packed.reduce((t, f) => t + f.data.length + f.rsrc.length, 0);
  const base = name.replace(/(\.(sit|sea|hqx|bin|cpt|zip))+$/i, "") || "Untitled";
  /* download sites put underscores for spaces */
  const volume = buildHfs(stringToMac(base.replace(/_/g, " ").replace(/:/g, "-")), tree, spare);
  log(name + ": " + all.length + " file(s) put on a " + (volume.length <= 1474560 ? (volume.length >> 10) + "K disk" : (volume.length >> 20) + " MB disk"));
  return [{ name: base.replace(/[\\/:*?"<>|]/g, "_") + ".dsk", data: volume }];
}

const converter = {
  description: "Mac archives (.SIT, .SEA, .CPT, .HQX, .BIN)",
  extensions: [".sit", ".sea", ".cpt", ".hqx", ".bin"],
  convert,
};

if (typeof window !== "undefined" && window.registerConverter) window.registerConverter(converter);
if (typeof module !== "undefined") module.exports = { ...converter, open, buildHfs, macToString, MacFileError, methodsSeen };
})();
