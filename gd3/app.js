const fileInput = document.getElementById("fileInput");
const dropZone = document.getElementById("dropZone");
const saveBtn = document.getElementById("saveBtn");
const saveAsVgz = document.getElementById("saveAsVgz");
const keepInputs = document.getElementById("keepInputs");
const statusEl = document.getElementById("status");

const filePathInput = document.getElementById("filePath");
const fileSizeEl = document.getElementById("fileSize");

const headerFields = {
  eofOffset: document.getElementById("eofOffset"),
  version: document.getElementById("version"),
  rate: document.getElementById("rate"),
  totalSamples: document.getElementById("totalSamples"),
  gd3Offset: document.getElementById("gd3Offset"),
  loopOffset: document.getElementById("loopOffset"),
  loopSamples: document.getElementById("loopSamples"),
  vgmDataOffset: document.getElementById("vgmDataOffset"),
  extraHeaderOffset: document.getElementById("extraHeaderOffset"),
};

const playbackFields = {
  volumeModifier: document.getElementById("volumeModifier"),
  loopBase: document.getElementById("loopBase"),
  loopModifier: document.getElementById("loopModifier"),
};

const chipFlagFields = {
  sn76489Feedback: document.getElementById("sn76489Feedback"),
  sn76489Shift: document.getElementById("sn76489Shift"),
  sn76489Flags: document.getElementById("sn76489Flags"),
  segaPcmInterface: document.getElementById("segaPcmInterface"),
  ay8910Type: document.getElementById("ay8910Type"),
  ay8910Flags: document.getElementById("ay8910Flags"),
  ym2203AyFlags: document.getElementById("ym2203AyFlags"),
  ym2608AyFlags: document.getElementById("ym2608AyFlags"),
  okim6258Flags: document.getElementById("okim6258Flags"),
  k054539Flags: document.getElementById("k054539Flags"),
  c140Type: document.getElementById("c140Type"),
  es5503Channels: document.getElementById("es5503Channels"),
  es5505Channels: document.getElementById("es5505Channels"),
  c352Divider: document.getElementById("c352Divider"),
};

const clockFields = {
  SN76489: document.getElementById("clkSN76489"),
  YM2413: document.getElementById("clkYM2413"),
  YM2612: document.getElementById("clkYM2612"),
  YM2151: document.getElementById("clkYM2151"),
  YM2203: document.getElementById("clkYM2203"),
  YM2608: document.getElementById("clkYM2608"),
  AY8910: document.getElementById("clkAY8910"),
  YM3812: document.getElementById("clkYM3812"),
  YM3526: document.getElementById("clkYM3526"),
  SegaPCM: document.getElementById("clkSegaPCM"),
  RF5C68: document.getElementById("clkRF5C68"),
  YM2610: document.getElementById("clkYM2610"),
  Y8950: document.getElementById("clkY8950"),
  YMF262: document.getElementById("clkYMF262"),
  YMF278B: document.getElementById("clkYMF278B"),
  YMF271: document.getElementById("clkYMF271"),
  YMZ280B: document.getElementById("clkYMZ280B"),
  RF5C164: document.getElementById("clkRF5C164"),
  PWM: document.getElementById("clkPWM"),
  GameBoy: document.getElementById("clkGameBoy"),
  NESAPU: document.getElementById("clkNESAPU"),
  MultiPCM: document.getElementById("clkMultiPCM"),
  UPD7759: document.getElementById("clkUPD7759"),
  OKIM6258: document.getElementById("clkOKIM6258"),
  OKIM6295: document.getElementById("clkOKIM6295"),
  K051649: document.getElementById("clkK051649"),
  K054539: document.getElementById("clkK054539"),
  HuC6280: document.getElementById("clkHuC6280"),
  C140: document.getElementById("clkC140"),
  K053260: document.getElementById("clkK053260"),
  Pokey: document.getElementById("clkPokey"),
  QSound: document.getElementById("clkQSound"),
  SCSP: document.getElementById("clkSCSP"),
  WonderSwan: document.getElementById("clkWonderSwan"),
  VSU: document.getElementById("clkVSU"),
  SAA1099: document.getElementById("clkSAA1099"),
  ES5503: document.getElementById("clkES5503"),
  ES5505: document.getElementById("clkES5505"),
  X1010: document.getElementById("clkX1010"),
  C352: document.getElementById("clkC352"),
  GA20: document.getElementById("clkGA20"),
  Mikey: document.getElementById("clkMikey"),
};

const gd3Fields = Array.from({ length: 11 }, (_, i) =>
  document.getElementById(`gd3-${i}`)
);

const state = {
  file: null,
  fileName: "",
  baseName: "",
  buffer: null,
  gd3offset: 0,
  isChanged: false,
  isCompressedSource: false,
};

function setStatus(message, type = "info") {
  statusEl.textContent = message;
  statusEl.classList.remove("warn", "error");
  if (type === "warn") {
    statusEl.classList.add("warn");
  } else if (type === "error") {
    statusEl.classList.add("error");
  }
}

function toHex(value, width = 0) {
  if (!Number.isFinite(value)) return "-";
  const hex = Math.trunc(value).toString(16).toUpperCase();
  const padded = width > 0 ? hex.padStart(width, "0") : hex;
  return `0x${padded}`;
}

function formatHex(width) {
  return (value) => toHex(value, width);
}

function resetHeaderFields() {
  Object.values(headerFields).forEach((el) => {
    el.textContent = "-";
  });
  Object.values(playbackFields).forEach((el) => {
    el.textContent = "-";
  });
  Object.values(chipFlagFields).forEach((el) => {
    el.textContent = "-";
  });
  Object.values(clockFields).forEach((el) => {
    el.textContent = "-";
  });
}

function setClock(field, value) {
  clockFields[field].textContent = value > 0 ? value.toString() : "0";
}

function setClockIfPresent(field, dataView, offset, headerLimit) {
  if (headerLimit >= offset + 4) {
    setClock(field, readUInt32LE(dataView, offset));
  } else {
    clockFields[field].textContent = "-";
  }
}

function setFieldIfPresent(el, dataView, headerLimit, offset, size, reader, formatter) {
  if (!el) return;
  if (headerLimit >= offset + size) {
    const value = reader(dataView, offset);
    el.textContent = formatter ? formatter(value) : value.toString();
  } else {
    el.textContent = "-";
  }
}

function markChanged() {
  state.isChanged = true;
  updateSaveState();
}

function updateSaveState() {
  const hasFormatChanged = state.file && saveAsVgz.checked !== state.isCompressedSource;
  saveBtn.disabled = !(state.file && (state.isChanged || hasFormatChanged));
}

function toggleSectionVisibility(button) {
  const targetId = button.getAttribute("aria-controls");
  if (!targetId) return;
  const target = document.getElementById(targetId);
  if (!target) return;
  const isExpanded = button.getAttribute("aria-expanded") === "true";
  const nextState = !isExpanded;
  button.setAttribute("aria-expanded", String(nextState));
  target.classList.toggle("is-hidden", !nextState);
}

function readUInt32LE(dataView, offset) {
  if (offset + 4 > dataView.byteLength) return 0;
  return dataView.getUint32(offset, true);
}

function readUInt16LE(dataView, offset) {
  if (offset + 2 > dataView.byteLength) return 0;
  return dataView.getUint16(offset, true);
}

function readUInt8(dataView, offset) {
  if (offset + 1 > dataView.byteLength) return 0;
  return dataView.getUint8(offset);
}

function readInt8(dataView, offset) {
  if (offset + 1 > dataView.byteLength) return 0;
  return dataView.getInt8(offset);
}

function isGzipBuffer(buffer) {
  return buffer.length >= 2 && buffer[0] === 0x1f && buffer[1] === 0x8b;
}

function hasVgmSignature(buffer) {
  return (
    buffer.length >= 4 &&
    buffer[0] === 0x56 &&
    buffer[1] === 0x67 &&
    buffer[2] === 0x6d &&
    buffer[3] === 0x20
  );
}

function deriveBaseName(fileName) {
  const lower = fileName.toLowerCase();
  if (lower.endsWith(".vgm.gz")) return fileName.slice(0, -7);
  if (lower.endsWith(".vgz")) return fileName.slice(0, -4);
  if (lower.endsWith(".vgm")) return fileName.slice(0, -4);
  if (lower.endsWith(".gz")) return fileName.slice(0, -3);
  return fileName;
}

function getSaveFileName() {
  return `${state.baseName}${saveAsVgz.checked ? ".vgz" : ".vgm"}`;
}

async function transformGzip(buffer, mode) {
  const StreamClass = mode === "compress" ? CompressionStream : DecompressionStream;
  if (typeof StreamClass !== "function") {
    throw new Error(`Stream API unavailable for ${mode}`);
  }
  const format = "gzip";
  const stream = new Blob([buffer]).stream().pipeThrough(new StreamClass(format));
  const arrayBuffer = await new Response(stream).arrayBuffer();
  return new Uint8Array(arrayBuffer);
}

function formatFileSizeText(rawSize, expandedSize, isCompressed) {
  if (isCompressed) {
    return `${rawSize} bytes (VGZ) / ${expandedSize} bytes (展開後)`;
  }
  return `${expandedSize} bytes`;
}

function parseGd3Strings(dataView, startOffset) {
  const fields = [];
  let offset = startOffset + 12;

  for (let i = 0; i < 11; i += 1) {
    const chars = [];
    while (offset + 2 <= dataView.byteLength) {
      const codeUnit = readUInt16LE(dataView, offset);
      offset += 2;
      if (codeUnit === 0) {
        break;
      }
      if (codeUnit === 0x000a || codeUnit === 0x0a00) {
        chars.push("\n");
      } else {
        chars.push(String.fromCharCode(codeUnit));
      }
    }
    fields.push(chars.join("").trim());
  }

  return fields;
}

async function loadFile(file) {
  if (!file) return;

  const rawArrayBuffer = await file.arrayBuffer();
  const rawBuffer = new Uint8Array(rawArrayBuffer);
  const sourceIsGzip = isGzipBuffer(rawBuffer);
  let expandedBuffer = rawBuffer;

  if (sourceIsGzip) {
    try {
      expandedBuffer = await transformGzip(rawBuffer, "decompress");
    } catch (error) {
      setStatus("gzip 展開に失敗しました。対応している VGZ ファイルを確認してください。", "error");
      console.error(error);
      return;
    }
  }

  if (!hasVgmSignature(expandedBuffer)) {
    setStatus("VGM または VGZ ファイルを選択してください。", "warn");
    return;
  }

  const dataView = new DataView(
    expandedBuffer.buffer,
    expandedBuffer.byteOffset,
    expandedBuffer.byteLength
  );
  state.file = file;
  state.fileName = file.name;
  state.baseName = deriveBaseName(file.name);
  state.buffer = expandedBuffer;
  state.isChanged = false;
  state.isCompressedSource = sourceIsGzip;
  saveAsVgz.checked = sourceIsGzip;

  filePathInput.value = file.name;
  fileSizeEl.textContent = formatFileSizeText(rawBuffer.length, state.buffer.length, sourceIsGzip);

  resetHeaderFields();

  try {
    const eofOffset = readUInt32LE(dataView, 0x04);
    const version = readUInt32LE(dataView, 0x08);
    const totalSamples = readUInt32LE(dataView, 0x18);
    const loopOffset = readUInt32LE(dataView, 0x1c);
    const loopSamples = readUInt32LE(dataView, 0x20);
    const gd3offset = readUInt32LE(dataView, 0x14) + 0x14;
    const dataOffset = version >= 0x150 ? readUInt32LE(dataView, 0x34) + 0x34 : 0x40;
    const headerLimit = Math.min(dataOffset, state.buffer.length);

    state.gd3offset = gd3offset;

    headerFields.eofOffset.textContent = toHex(eofOffset);
    headerFields.version.textContent = toHex(version);
    setFieldIfPresent(
      headerFields.rate,
      dataView,
      headerLimit,
      0x24,
      4,
      readUInt32LE,
      (value) => value.toString()
    );
    headerFields.totalSamples.textContent = totalSamples.toString();
    headerFields.gd3Offset.textContent = toHex(gd3offset);
    headerFields.loopOffset.textContent = toHex(loopOffset);
    headerFields.loopSamples.textContent = loopSamples.toString();
    headerFields.vgmDataOffset.textContent = toHex(dataOffset);
    setFieldIfPresent(
      headerFields.extraHeaderOffset,
      dataView,
      headerLimit,
      0xbc,
      4,
      readUInt32LE,
      (value) => toHex(value, 8)
    );

    setFieldIfPresent(
      playbackFields.volumeModifier,
      dataView,
      headerLimit,
      0x7c,
      1,
      readInt8,
      (value) => value.toString()
    );
    setFieldIfPresent(
      playbackFields.loopBase,
      dataView,
      headerLimit,
      0x7e,
      1,
      readInt8,
      (value) => value.toString()
    );
    setFieldIfPresent(
      playbackFields.loopModifier,
      dataView,
      headerLimit,
      0x7f,
      1,
      readUInt8,
      formatHex(2)
    );

    setFieldIfPresent(
      chipFlagFields.sn76489Feedback,
      dataView,
      headerLimit,
      0x28,
      2,
      readUInt16LE,
      formatHex(4)
    );
    setFieldIfPresent(
      chipFlagFields.sn76489Shift,
      dataView,
      headerLimit,
      0x2a,
      1,
      readUInt8,
      (value) => value.toString()
    );
    setFieldIfPresent(
      chipFlagFields.sn76489Flags,
      dataView,
      headerLimit,
      0x2b,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.segaPcmInterface,
      dataView,
      headerLimit,
      0x3c,
      4,
      readUInt32LE,
      formatHex(8)
    );
    setFieldIfPresent(
      chipFlagFields.ay8910Type,
      dataView,
      headerLimit,
      0x78,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.ay8910Flags,
      dataView,
      headerLimit,
      0x79,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.ym2203AyFlags,
      dataView,
      headerLimit,
      0x7a,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.ym2608AyFlags,
      dataView,
      headerLimit,
      0x7b,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.okim6258Flags,
      dataView,
      headerLimit,
      0x94,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.k054539Flags,
      dataView,
      headerLimit,
      0x95,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.c140Type,
      dataView,
      headerLimit,
      0x96,
      1,
      readUInt8,
      formatHex(2)
    );
    setFieldIfPresent(
      chipFlagFields.es5503Channels,
      dataView,
      headerLimit,
      0xd4,
      1,
      readUInt8,
      (value) => value.toString()
    );
    setFieldIfPresent(
      chipFlagFields.es5505Channels,
      dataView,
      headerLimit,
      0xd5,
      1,
      readUInt8,
      (value) => value.toString()
    );
    setFieldIfPresent(
      chipFlagFields.c352Divider,
      dataView,
      headerLimit,
      0xd6,
      1,
      readUInt8,
      (value) => value.toString()
    );

    [
      ["SN76489", 0x0c],
      ["YM2413", 0x10],
      ["YM2612", 0x2c],
      ["YM2151", 0x30],
      ["SegaPCM", 0x38],
      ["RF5C68", 0x40],
      ["YM2203", 0x44],
      ["YM2608", 0x48],
      ["YM2610", 0x4c],
      ["YM3812", 0x50],
      ["YM3526", 0x54],
      ["Y8950", 0x58],
      ["YMF262", 0x5c],
      ["YMF278B", 0x60],
      ["YMF271", 0x64],
      ["YMZ280B", 0x68],
      ["RF5C164", 0x6c],
      ["PWM", 0x70],
      ["AY8910", 0x74],
      ["GameBoy", 0x80],
      ["NESAPU", 0x84],
      ["MultiPCM", 0x88],
      ["UPD7759", 0x8c],
      ["OKIM6258", 0x90],
      ["OKIM6295", 0x98],
      ["K051649", 0x9c],
      ["K054539", 0xa0],
      ["HuC6280", 0xa4],
      ["C140", 0xa8],
      ["K053260", 0xac],
      ["Pokey", 0xb0],
      ["QSound", 0xb4],
      ["SCSP", 0xb8],
      ["WonderSwan", 0xc0],
      ["VSU", 0xc4],
      ["SAA1099", 0xc8],
      ["ES5503", 0xcc],
      ["ES5505", 0xd0],
      ["X1010", 0xd8],
      ["C352", 0xdc],
      ["GA20", 0xe0],
      ["Mikey", 0xe4],
    ].forEach(([field, offset]) => {
      setClockIfPresent(field, dataView, offset, headerLimit);
    });

    const hasGd3 =
      gd3offset + 12 <= state.buffer.length &&
      state.buffer[gd3offset] === 0x47 &&
      state.buffer[gd3offset + 1] === 0x64 &&
      state.buffer[gd3offset + 2] === 0x33 &&
      state.buffer[gd3offset + 3] === 0x20;

    if (hasGd3) {
      if (!keepInputs.checked) {
        const gd3Values = parseGd3Strings(dataView, gd3offset);
        gd3Values.forEach((value, i) => {
          if (gd3Fields[i]) gd3Fields[i].value = value;
        });
      }
      setStatus(
        sourceIsGzip
          ? "VGZ を展開して読み込みました。GD3 タグを編集できます。"
          : "読み込み完了。GD3 タグを編集できます。"
      );
    } else {
      setStatus(
        sourceIsGzip
          ? "VGZ を展開しましたが GD3 タグが見つかりませんでした。保存すると既存データを上書きする可能性があります。"
          : "GD3 タグが見つかりませんでした。保存すると既存データを上書きする可能性があります。",
        "warn"
      );
    }
  } catch (error) {
    setStatus("ファイル解析に失敗しました。別の VGM ファイルを試してください。", "error");
    console.error(error);
  }

  updateSaveState();
}

function normalizeText(value) {
  return value.trim().replace(/\r/g, "");
}

function writeUInt32LE(buffer, offset, value) {
  buffer[offset] = value & 0xff;
  buffer[offset + 1] = (value >> 8) & 0xff;
  buffer[offset + 2] = (value >> 16) & 0xff;
  buffer[offset + 3] = (value >> 24) & 0xff;
}

async function saveFile() {
  if (!state.file || !state.buffer) {
    setStatus("保存する前にファイルを読み込んでください。", "warn");
    return;
  }

  const gd3Bytes = [];
  gd3Fields.forEach((field) => {
    const text = normalizeText(field.value);
    for (let i = 0; i < text.length; i += 1) {
      const codeUnit = text.charCodeAt(i);
      gd3Bytes.push(codeUnit & 0xff, (codeUnit >> 8) & 0xff);
    }
    gd3Bytes.push(0x00, 0x00);
  });

  const gd3Size = gd3Bytes.length;
  const gd3offset = state.gd3offset;

  if (!Number.isFinite(gd3offset) || gd3offset < 0x14) {
    setStatus("GD3 オフセットが不正です。", "error");
    return;
  }

  const newFileSize = gd3offset + 12 + gd3Size;
  const out = new Uint8Array(newFileSize);
  out.set(state.buffer.slice(0, gd3offset), 0);

  out[gd3offset] = 0x47;
  out[gd3offset + 1] = 0x64;
  out[gd3offset + 2] = 0x33;
  out[gd3offset + 3] = 0x20;
  out[gd3offset + 4] = 0x00;
  out[gd3offset + 5] = 0x01;
  out[gd3offset + 6] = 0x00;
  out[gd3offset + 7] = 0x00;
  writeUInt32LE(out, gd3offset + 8, gd3Size);

  out.set(gd3Bytes, gd3offset + 12);

  const newEofOffset = newFileSize - 0x04;
  writeUInt32LE(out, 0x04, newEofOffset);

  let downloadBytes = out;
  if (saveAsVgz.checked) {
    try {
      downloadBytes = await transformGzip(out, "compress");
    } catch (error) {
      setStatus("このブラウザでは VGZ 保存に必要な gzip 圧縮が使えません。", "error");
      console.error(error);
      return;
    }
  }

  const blob = new Blob([downloadBytes], { type: "application/octet-stream" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = getSaveFileName();
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);

  state.buffer = out;
  state.fileName = getSaveFileName();
  state.isChanged = false;
  state.isCompressedSource = saveAsVgz.checked;
  updateSaveState();
  filePathInput.value = state.fileName;
  fileSizeEl.textContent = formatFileSizeText(downloadBytes.length, out.length, saveAsVgz.checked);
  setStatus(saveAsVgz.checked ? "VGZ ファイルをダウンロードしました。" : "VGM ファイルをダウンロードしました。");
}

fileInput.addEventListener("change", (event) => {
  const file = event.target.files[0];
  loadFile(file);
});

saveBtn.addEventListener("click", () => {
  saveFile();
});

saveAsVgz.addEventListener("change", () => {
  updateSaveState();
});

gd3Fields.forEach((field) => {
  field.addEventListener("input", markChanged);
});

document.addEventListener("click", (event) => {
  const header = event.target.closest(".section-header");
  if (!header) return;
  const button = header.querySelector(".section-toggle");
  if (!button) return;
  toggleSectionVisibility(button);
});

dropZone.addEventListener("dragover", (event) => {
  event.preventDefault();
  dropZone.classList.add("dragover");
});

dropZone.addEventListener("dragleave", () => {
  dropZone.classList.remove("dragover");
});

dropZone.addEventListener("drop", (event) => {
  event.preventDefault();
  dropZone.classList.remove("dragover");
  const file = event.dataTransfer.files[0];
  loadFile(file);
});
