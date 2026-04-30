(function () {
  // ----------------------------
  // UI helpers
  // ----------------------------
  const logEl = () => document.getElementById("cfLog");

  function log(line) {
    const el = logEl();
    el.textContent += line + "\n";
    el.scrollTop = el.scrollHeight;
  }

  async function fetchJson(url) {
    const r = await fetch(url, { cache: "no-store" });
    if (!r.ok) throw new Error(`${url} failed: ${r.status}`);
    return await r.json();
  }

  async function postJson(url, bodyObj) {
    const r = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(bodyObj),
    });
    const txt = await r.text();
    if (!r.ok) throw new Error(`${url} failed: ${r.status} ${txt}`);
    return txt;
  }

  // Yield to allow UI repaint
  async function yieldUi() {
    await new Promise((r) => setTimeout(r, 0));
  }

  // ----------------------------
  // WebSocket upload (START/READY/BINARY/PROGRESS/DONE protocol)
  // ----------------------------
  async function getWsUrl() {
    const status = await fetchJson("/api/status");
    const host = window.location.hostname;
    const wsPort = status.wsPort;
    if (!wsPort) throw new Error('Missing wsPort in /api/status (add doc["wsPort"]=wsPort in firmware)');
    return `ws://${host}:${wsPort}/`;
  }

  async function wsUploadFileBytes(wsUrl, path, filename, bytes) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl);
      ws.binaryType = "arraybuffer";

      const total = bytes.byteLength;
      const startMsg = `START:${filename}:${total}:${path}`;

      ws.onopen = () => ws.send(startMsg);
      ws.onerror = () => reject(new Error("WebSocket error"));

      ws.onmessage = (ev) => {
        if (typeof ev.data !== "string") return;
        const msg = ev.data;

        if (msg === "READY") {
          const chunkSize = 32 * 1024;
          let off = 0;

          const sendNext = () => {
            if (off >= total) return;
            const end = Math.min(off + chunkSize, total);
            ws.send(bytes.slice(off, end));
            off = end;
            setTimeout(sendNext, 0);
          };
          sendNext();
          return;
        }

        if (msg.startsWith("PROGRESS:")) {
          log(msg);
          return;
        }

        if (msg === "DONE") {
          ws.close();
          resolve();
          return;
        }

        if (msg.startsWith("ERROR:")) {
          ws.close();
          reject(new Error(msg));
          return;
        }
      };
    });
  }

  function buildBinFilename(slot, sizePx, style) {
    return `custom_slot${slot}_${sizePx}_${style}.bin`;
  }

  // ----------------------------
  // CPFN v1 constants (match CustomFontBinFormat.h)
  // ----------------------------
  const CPFN_MAGIC = 0x4e465043; // 'CPFN' as LE uint32
  const VER_MAJOR = 1;
  const VER_MINOR = 0;
  const HEADER_SIZE = 146;

  // Flags (FontFlags)
  const FLAG_IS_2BIT = 1 << 0;
  const FLAG_HAS_GROUPS = 1 << 1;

  // Record sizes (packed)
  const GLYPH_REC_SIZE = 14; // GlyphRecV1
  const INTERVAL_REC_SIZE = 12; // IntervalRecV1
  const GROUP_REC_SIZE = 18; // GroupRecV1

  // ----------------------------
  // Raw deflate (browser-native)
  // ----------------------------
  function deflateRawStored(u8) {
    // Raw DEFLATE "stored" blocks (BTYPE=00), byte-aligned.
    // Each block: 1 byte header (0x00 or 0x01), then LEN(2), NLEN(2), then data.
    // Split into <= 65535 chunks (DEFLATE stored block limit).

    const CHUNK = 65535;
    const n = u8.byteLength;

    // Special-case empty payload (valid DEFLATE stream)
    if (n === 0) {
      return new Uint8Array([0x01, 0x00, 0x00, 0xff, 0xff]);
    }

    const blockCount = Math.ceil(n / CHUNK);
    const outLen = n + blockCount * 5;
    const out = new Uint8Array(outLen);

    let srcOff = 0;
    let dstOff = 0;

    for (let bi = 0; bi < blockCount; bi++) {
      const remaining = n - srcOff;
      const len = Math.min(CHUNK, remaining);
      const finalBlock = bi === blockCount - 1;

      // BFINAL + BTYPE=00 => 0x01 for final, 0x00 for non-final
      out[dstOff++] = finalBlock ? 0x01 : 0x00;

      // LEN (little-endian)
      out[dstOff++] = len & 0xff;
      out[dstOff++] = (len >>> 8) & 0xff;

      // NLEN = one's complement of LEN (16-bit)
      const nlen = (~len) & 0xffff;
      out[dstOff++] = nlen & 0xff;
      out[dstOff++] = (nlen >>> 8) & 0xff;

      // Data
      out.set(u8.subarray(srcOff, srcOff + len), dstOff);
      srcOff += len;
      dstOff += len;
    }

    return out;
  }

  // Use stored blocks for maximum compatibility (no CompressionStream needed)
  async function deflateRaw(u8) {
    return deflateRawStored(u8);
  }

  // ----------------------------
  // Charset policies (contiguous intervals)
  // ----------------------------
  function getCoverageIntervals(mode) {
    // Minimal: still reasonably small, but now includes General Punctuation so EPUB quotes/dashes work.
    const minimal = [
      [0x0020, 0x007e], // Basic Latin printable
      [0x00a0, 0x00ff], // Latin-1 supplement (no controls)
      [0x2000, 0x206f], // General punctuation (smart quotes, dashes, ellipsis, etc.)
      [0xfffd, 0xfffd], // replacement glyph
    ];

    // Extended (book-friendly): most European languages + punctuation + currency
    // (Intentionally excludes arrows/math/shapes/symbol blocks because generation cost is huge.)
    const extended = [
      [0x0020, 0x007e],
      [0x00a0, 0x00ff],
      [0x0100, 0x017f], // Latin Extended-A
      [0x0180, 0x024f], // Latin Extended-B
      [0x1e00, 0x1eff], // Latin Extended Additional
      [0x2000, 0x206f], // General punctuation
      [0x20a0, 0x20cf], // Currency
      [0xfffd, 0xfffd],
    ];

    return mode === "extended" ? extended : minimal;
  }

  // ----------------------------
  // Rasterize one codepoint to 2-bit byte-aligned rows
  // ----------------------------
  function quantizeAlphaTo2Bit(a) {
    // a: 0..255 => 2-bit 0..3
    if (a < 32) return 0;
    if (a < 96) return 1;
    if (a < 160) return 2;
    return 3;
  }

  function packAligned2Bit(pixels2bit, width, height) {
    // Byte-aligned rows: rowStride = ceil(width/4)
    const rowStride = Math.floor((width + 3) / 4);
    const out = new Uint8Array(rowStride * height);
    let p = 0;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const v = pixels2bit[p++] & 0x3;
        const byteIndex = y * rowStride + Math.floor(x / 4);
        const shift = (3 - (x % 4)) * 2;
        out[byteIndex] |= v << shift;
      }
    }
    return out;
  }

  function hashU8(u8) {
    // Tiny non-crypto hash (FNV-1a-ish) for missing-glyph detection
    let h = 2166136261 >>> 0;
    for (let i = 0; i < u8.length; i++) {
      h ^= u8[i];
      h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
  }

  function renderGlyphToBitmap(ctx, fontCss, cp, baselineX, baselineY, canvasW, canvasH) {
  const s = String.fromCodePoint(cp);

  ctx.clearRect(0, 0, canvasW, canvasH);
  ctx.font = fontCss;
  ctx.textBaseline = "alphabetic";
  ctx.fillStyle = "black";
  ctx.fillText(s, baselineX, baselineY);

  const img = ctx.getImageData(0, 0, canvasW, canvasH);
  const data = img.data;

  // Scan bounding box of alpha>0
  let minX = canvasW, minY = canvasH, maxX = -1, maxY = -1;
  for (let y = 0; y < canvasH; y++) {
    for (let x = 0; x < canvasW; x++) {
      const a = data[(y * canvasW + x) * 4 + 3];
      if (a !== 0) {
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
      }
    }
  }

  const tm = ctx.measureText(s);
  const advancePx = tm.width;

  // Empty glyph (space, etc.)
  if (maxX < minX || maxY < minY) {
    return {
      width: 0,
      height: 0,
      left: 0,
      top: 0,
      advancePx,
      aligned: new Uint8Array(0),
      alignedHash: 0,
    };
  }

  const width = maxX - minX + 1;
  const height = maxY - minY + 1;

  // CRITICAL: Compute left and top relative to baseline point
  // left = pixels from baseline horizontally (can be negative for italic)
  // top = pixels ABOVE baseline (always positive)
  const left = minX - baselineX;
  const top = baselineY - minY;  // distance from baseline UP to top of glyph

  // Extract bbox pixels -> 2-bit
  const pixels2bit = new Uint8Array(width * height);
  let p = 0;
  for (let y = 0; y < height; y++) {
    const yy = minY + y;
    for (let x = 0; x < width; x++) {
      const xx = minX + x;
      const a = data[(yy * canvasW + xx) * 4 + 3];
      pixels2bit[p++] = quantizeAlphaTo2Bit(a);
    }
  }

  const aligned = packAligned2Bit(pixels2bit, width, height);
  const alignedHash = (hashU8(aligned) ^ (width << 16) ^ height) >>> 0;

  return { width, height, left, top, advancePx, aligned, alignedHash };
}

  function styleToFontCssPrefix(style) {
    // Canvas font syntax: "[style] [weight] <size>px <family>"
    switch (style) {
      case "bold":
        return "normal 700";
      case "italic":
        return "italic 400";
      case "bolditalic":
        return "italic 700";
      case "regular":
      default:
        return "normal 400";
    }
  }

function computeHeaderMetrics(ctx, sizePx) {
  // Use a string that contains ascenders + descenders for robust metrics.
  const sample = "Hg|Áyjpq";
  const m = ctx.measureText(sample);

  // actualBoundingBoxAscent: distance from baseline UP to tallest pixel
  // actualBoundingBoxDescent: distance from baseline DOWN to lowest pixel
  const asc = Math.ceil(m.actualBoundingBoxAscent || sizePx * 0.8);
  const desc = Math.ceil(m.actualBoundingBoxDescent || sizePx * 0.25);

  // advanceY = line height = space needed from one baseline to the next
  // CRITICAL: Must be at least asc + desc + leading
  const advanceY = Math.min(255, Math.max(1, asc + desc + 2));

  console.log(`Metrics: size=${sizePx}px, asc=${asc}, desc=${desc}, advanceY=${advanceY}`);
  return { asc, desc, advanceY };
}

  // ----------------------------
  // Build CPFN v1 (.bin) for a font file and a single size+style
  // ----------------------------
  async function generateCpfntBinFromFontFile(ttfArrayBuffer, sizePx, style, charsetMode) {
    log(`Loading FontFace @ ${sizePx}px (${style})...`);

    // Load the font into the browser
    const family = "CF_" + Math.random().toString(16).slice(2);
    const face = new FontFace(family, ttfArrayBuffer);
    await face.load();
    document.fonts.add(face);

    log(`FontFace loaded. Raster setup...`);
    await yieldUi();

    // Canvas for rasterization
    const canvas = document.createElement("canvas");
    const canvasW = 256;
    const canvasH = 256;
    canvas.width = canvasW;
    canvas.height = canvasH;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });

    const cssPrefix = styleToFontCssPrefix(style);
    const fontCss = `${cssPrefix} ${sizePx}px "${family}"`;

    // Baseline point (leave margin for negative left/top)
    const baselineX = 64;
    const baselineY = 128;

    // Header metrics
    ctx.font = fontCss;
    ctx.textBaseline = "alphabetic";
    const { asc, desc, advanceY } = computeHeaderMetrics(ctx, sizePx);
    log(`Metrics: asc=${asc} desc=${desc} advanceY=${advanceY}`);

    // Coverage intervals -> codepoint list
    const intervals = getCoverageIntervals(charsetMode);

    const codepoints = [];
    for (const [a, b] of intervals) {
      for (let cp = a; cp <= b; cp++) codepoints.push(cp);
    }

    // Missing-glyph detection
    const missingProbeCp = 0x10ffff;
    const missingProbe = renderGlyphToBitmap(ctx, fontCss, missingProbeCp, baselineX, baselineY, canvasW, canvasH);

    // Replacement glyph U+FFFD
    const replCp = 0xfffd;
    const repl = renderGlyphToBitmap(ctx, fontCss, replCp, baselineX, baselineY, canvasW, canvasH);

    // Build glyph records and aligned bitmaps (byte-aligned rows)
    const glyphs = new Array(codepoints.length);
    const alignedBitmaps = new Array(codepoints.length);

    log(`Rasterizing ${codepoints.length} glyphs @ ${sizePx}px...`);
    await yieldUi();

    for (let i = 0; i < codepoints.length; i++) {
      const cp = codepoints[i];
      let g = renderGlyphToBitmap(ctx, fontCss, cp, baselineX, baselineY, canvasW, canvasH);

      if (g.alignedHash === missingProbe.alignedHash) g = repl;

      // IMPORTANT:
      // We store bitmaps as byte-aligned rows => data length must equal rowStride*height,
      // which is exactly g.aligned.length (NOT ceil(w*h/4)).
      const dataLen = g.aligned ? g.aligned.length : 0;

      glyphs[i] = {
        width: g.width,
        height: g.height,
        left: g.left,
        top: g.top,
        advanceX_fp4: Math.max(0, Math.min(0xffff, Math.round(g.advancePx * 16))), // 12.4 fixed
        dataLength: dataLen,
        dataOffset: 0, // filled during table write
      };

      alignedBitmaps[i] = g.aligned;

      if (i !== 0 && i % 50 === 0) {
        log(`... ${i}/${codepoints.length} glyphs`);
        await yieldUi();
      }
    }

    log(`Rasterizing done. Grouping...`);
    await yieldUi();

    // Grouping: contiguous groups with uncompressed cap
    const MAX_GROUP_UNCOMP = 48 * 1024;

    const groups = [];
    let groupStart = 0;
    while (groupStart < glyphs.length) {
      let groupEnd = groupStart;
      let uncomp = 0;

      while (groupEnd < glyphs.length) {
        const add = glyphs[groupEnd].dataLength;
        if (groupEnd > groupStart && uncomp + add > MAX_GROUP_UNCOMP) break;
        uncomp += add;
        groupEnd++;
      }

      groups.push({
        firstGlyphIndex: groupStart,
        glyphCount: groupEnd - groupStart,
        uncompressedSize: uncomp,
      });

      groupStart = groupEnd;
    }

    log(`Compressing ${groups.length} group(s) (raw deflate)...`);
    await yieldUi();

    // Compress groups and concatenate compressed streams
    const compressedChunks = [];
    for (let gi = 0; gi < groups.length; gi++) {
      const gr = groups[gi];

      const parts = [];
      let total = 0;
      for (let j = 0; j < gr.glyphCount; j++) {
        const idx = gr.firstGlyphIndex + j;
        const a = alignedBitmaps[idx];
        if (a && a.length) {
          parts.push(a);
          total += a.length;
        }
      }

      const payload = new Uint8Array(total);
      let off = 0;
      for (const part of parts) {
        payload.set(part, off);
        off += part.length;
      }
      log(`Deflating group ${gi + 1}/${groups.length}, payload=${payload.length} bytes`);
      await yieldUi();
      const deflated = await deflateRaw(payload);

      compressedChunks.push({
        compressed: deflated,
        compressedSize: deflated.length,
        uncompressedSize: payload.length,
        glyphCount: gr.glyphCount,
        firstGlyphIndex: gr.firstGlyphIndex,
      });

      if (gi !== 0 && gi % 4 === 0) {
        log(`... compressed ${gi}/${groups.length} groups`);
        await yieldUi();
      }
    }

    // Bitmap section = concat(compressed)
    let bitmapSize = 0;
    for (const c of compressedChunks) bitmapSize += c.compressed.length;
    const bitmapBlob = new Uint8Array(bitmapSize);

    let bitmapOff = 0;
    for (const c of compressedChunks) {
      c.compressedOffset = bitmapOff; // relative to bitmap section start
      bitmapBlob.set(c.compressed, bitmapOff);
      bitmapOff += c.compressed.length;
    }

    // Tables
    const glyphCount = glyphs.length;
    const intervalCount = intervals.length;
    const groupCount = compressedChunks.length;

    const bitmapOffset = HEADER_SIZE;
    const glyphTableOffset = bitmapOffset + bitmapBlob.length;
    const glyphTableSize = glyphCount * GLYPH_REC_SIZE;

    const intervalTableOffset = glyphTableOffset + glyphTableSize;
    const intervalTableSize = intervalCount * INTERVAL_REC_SIZE;

    const groupTableOffset = intervalTableOffset + intervalTableSize;
    const groupTableSize = groupCount * GROUP_REC_SIZE;

    const fileSize = groupTableOffset + groupTableSize;

    const buf = new ArrayBuffer(fileSize);
    const dv = new DataView(buf);
    const u8 = new Uint8Array(buf);

    // ----------------------------
    // HeaderV1 (packed, little endian)
    // ----------------------------
    let p = 0;
    dv.setUint32(p, CPFN_MAGIC, true);
    p += 4;
    dv.setUint8(p++, VER_MAJOR);
    dv.setUint8(p++, VER_MINOR);
    dv.setUint16(p, HEADER_SIZE, true);
    p += 2;

    dv.setUint32(p, FLAG_IS_2BIT | FLAG_HAS_GROUPS, true);
    p += 4;

    dv.setUint8(p++, advanceY);
    dv.setInt16(p, asc, true);
    p += 2;
    dv.setInt16(p, desc, true);  // Store as positive magnitude (not negated)
    p += 2;
    dv.setUint8(p++, 0); // reserved0

    dv.setUint32(p, glyphCount, true);
    p += 4;
    dv.setUint32(p, intervalCount, true);
    p += 4;
    dv.setUint32(p, groupCount, true);
    p += 4;

    // kerning/ligatures counts = 0
    dv.setUint16(p, 0, true);
    p += 2; // kernLeftEntryCount
    dv.setUint16(p, 0, true);
    p += 2; // kernRightEntryCount
    dv.setUint8(p++, 0); // kernLeftClassCount
    dv.setUint8(p++, 0); // kernRightClassCount
    dv.setUint16(p, 0, true);
    p += 2; // reserved1
    dv.setUint32(p, 0, true);
    p += 4; // ligaturePairCount

    dv.setUint32(p, bitmapOffset, true);
    p += 4;
    dv.setUint32(p, bitmapBlob.length, true);
    p += 4;

    dv.setUint32(p, glyphTableOffset, true);
    p += 4;
    dv.setUint32(p, glyphTableSize, true);
    p += 4;

    dv.setUint32(p, intervalTableOffset, true);
    p += 4;
    dv.setUint32(p, intervalTableSize, true);
    p += 4;

    dv.setUint32(p, groupTableOffset, true);
    p += 4;
    dv.setUint32(p, groupTableSize, true);
    p += 4;

    // glyphToGroup + kern + ligature sections absent (5 sections)
    for (let i = 0; i < 5; i++) {
      dv.setUint32(p, 0, true);
      p += 4; // offset
      dv.setUint32(p, 0, true);
      p += 4; // size
    }

    // reserved2[8]
    for (let i = 0; i < 8; i++) {
      dv.setUint32(p, 0, true);
      p += 4;
    }

    // Copy bitmap blob
    u8.set(bitmapBlob, bitmapOffset);

    // Glyph table
    let go = glyphTableOffset;
    let dataOffset = 0;
    for (let i = 0; i < glyphCount; i++) {
      const g = glyphs[i];
      dv.setUint8(go + 0, g.width);
      dv.setUint8(go + 1, g.height);
      dv.setUint16(go + 2, g.advanceX_fp4, true);
      dv.setInt16(go + 4, g.left, true);
      dv.setInt16(go + 6, g.top, true);
      dv.setUint16(go + 8, g.dataLength, true);
      dv.setUint32(go + 10, dataOffset, true);
      dataOffset += g.dataLength;
      go += GLYPH_REC_SIZE;
    }

    // Interval table
    let io = intervalTableOffset;
    let glyphIndex = 0;
    for (let i = 0; i < intervals.length; i++) {
      const first = intervals[i][0];
      const last = intervals[i][1];
      dv.setUint32(io + 0, first, true);
      dv.setUint32(io + 4, last, true);
      dv.setUint32(io + 8, glyphIndex, true);
      glyphIndex += last - first + 1;
      io += INTERVAL_REC_SIZE;
    }

    // Group table
    let gro = groupTableOffset;
    for (let i = 0; i < compressedChunks.length; i++) {
      const g = compressedChunks[i];
      dv.setUint32(gro + 0, g.compressedOffset, true);
      dv.setUint32(gro + 4, g.compressedSize, true);
      dv.setUint32(gro + 8, g.uncompressedSize, true);
      dv.setUint16(gro + 12, g.glyphCount, true);
      dv.setUint32(gro + 14, g.firstGlyphIndex, true);
      gro += GROUP_REC_SIZE;
    }

    // Clean up FontFace to avoid stacking many fonts in memory across runs
    try {
      document.fonts.delete(face);
    } catch (_) {
      // ignore
    }

    return buf;
  }

  // ----------------------------
  // Slot index update (styleMask bits)
  // bit0=regular, bit1=bold, bit2=italic, bit3=bolditalic
  // ----------------------------
  function styleToMaskBit(style) {
    if (style === "regular") return 1;
    if (style === "bold") return 2;
    if (style === "italic") return 4;
    if (style === "bolditalic") return 8;
    return 0;
  }

  // ----------------------------
  // UI actions
  // ----------------------------
  async function onGenerateUpload() {
    logEl().textContent = "";

    const slot = parseInt(document.getElementById("cfSlot").value, 10);
    const name = (document.getElementById("cfName").value || "").trim();
    const style = document.getElementById("cfStyle").value;
    const charsetMode = document.getElementById("cfCharset").value;
    const file = document.getElementById("cfTtf").files[0];

    if (!file) throw new Error("Please choose a .ttf/.otf file");
    if (!name) throw new Error("Please enter a display name");

    log(`Slot=${slot} Name="${name}" Style=${style} Charset=${charsetMode}`);
    log(`Reading font: ${file.name} (${file.size} bytes)`);

    const ttfBuf = await file.arrayBuffer();

    const wsUrl = await getWsUrl();
    log(`WS: ${wsUrl}`);

    const sizes = await fetchJson("/api/font_sizes"); // [10,12,14,16,18]
    log(`Sizes: ${JSON.stringify(sizes)}`);

    const uploadPath = "/.crosspoint/fonts";

    for (const sizePx of sizes) {
      log(`--- Generating size ${sizePx}px ---`);
      await yieldUi();

      const bin = await generateCpfntBinFromFontFile(ttfBuf, sizePx, style, charsetMode);
      const filename = buildBinFilename(slot, sizePx, style);

      log(`Uploading ${filename} (${bin.byteLength} bytes) → ${uploadPath}`);
      await wsUploadFileBytes(wsUrl, uploadPath, filename, bin);
      log(`OK: ${filename}`);
    }

    // Update index: preserve existing styleMask by OR-ing our bit
    const idx = await fetchJson("/api/custom_fonts");
    const slotObj = (idx.slots || []).find((s) => s.id === slot);
    const oldMask = slotObj ? (slotObj.styleMask | 0) : 0;
    const newMask = oldMask | styleToMaskBit(style);

    await postJson("/api/custom_fonts", {
      slot,
      name,
      styleMask: newMask,
      active: true,
    });

    log(`Index updated (styleMask ${oldMask} -> ${newMask}).`);
    log("Done. On device: Settings → Reader → Custom Font.");
  }

  async function onDeleteSlot() {
    logEl().textContent = "";
    const slot = parseInt(document.getElementById("cfSlot").value, 10);
    log(`Deleting slot ${slot}...`);
    const r = await fetch(`/api/delete_custom_font?slot=${slot}`, { method: "POST" });
    const t = await r.text();
    if (!r.ok) throw new Error(t);
    log("OK: slot deleted");
  }

  window.addEventListener("load", () => {
    const genBtn = document.getElementById("cfGenerateUploadBtn");
    const delBtn = document.getElementById("cfDeleteBtn");
    if (!genBtn || !delBtn) return;

    genBtn.addEventListener("click", async () => {
      try {
        await onGenerateUpload();
      } catch (e) {
        log("ERROR: " + (e && e.message ? e.message : String(e)));
      }
    });

    delBtn.addEventListener("click", async () => {
      try {
        await onDeleteSlot();
      } catch (e) {
        log("ERROR: " + (e && e.message ? e.message : String(e)));
      }
    });
  });
})();