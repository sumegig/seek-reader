#pragma once
#include <cstdint>
#include <cstddef>

// Custom Font Binary Format (CPFN) - v1
//
// Purpose:
//   A stable, versioned on-SD container for EpdFontData that can be loaded
//   into heap-allocated tables (glyphs/intervals/groups/kerning/ligatures)
//   and used by the existing rendering + FontDecompressor pipeline.
//
// Endianness:
//   All multi-byte fields are LITTLE-ENDIAN on disk.
//
// Bitmap section meaning:
//   - If flags.hasGroups == 0: bitmap is the *packed* glyph bitmap stream
//     (the same format as uncompressed EpdFontData->bitmap expects).
//   - If flags.hasGroups == 1: bitmap is a concatenation of raw-DEFLATE streams.
//     Each group's compressedOffset is relative to the START of the bitmap section.
//     After inflate, data is in BYTE-ALIGNED 2-bit rows:
//       alignedBytesPerGlyph = ((width + 3) / 4) * height
//     and the runtime compacts it back to packed form (FontDecompressor does this).
//
// Compatibility notes:
//   - We do NOT store pointers. We store tables as raw arrays and offsets.
//   - The firmware loader must allocate arrays and populate EpdFontData pointers.
//   - We store "serialized" structs to avoid compiler padding differences.
//
// Magic:
//   Bytes: 'C' 'P' 'F' 'N'  (0x43 0x50 0x46 0x4E)

namespace cpfnt {

static constexpr uint8_t  kVersionMajor = 1;
static constexpr uint8_t  kVersionMinor = 0;
static constexpr uint32_t kMagic = 0x4E465043u; // 'CPFN' as LE uint32

// ---- Flags ----
enum FontFlags : uint32_t {
  FLAG_IS_2BIT          = 1u << 0, // Matches EpdFontData::is2Bit
  FLAG_HAS_GROUPS       = 1u << 1, // groups + compressed bitmap streams present
  FLAG_HAS_GLYPH2GROUP  = 1u << 2, // glyphToGroup table present (frequency-grouped fonts)
  FLAG_HAS_KERNING      = 1u << 3, // kern classes + matrix present
  FLAG_HAS_LIGATURES    = 1u << 4, // ligaturePairs present
};

// ---- Sections ----
// Offsets are file-relative byte offsets from the start of the file.
struct __attribute__((packed)) HeaderV1 {
  // 0x00
  uint32_t magic;        // kMagic
  uint8_t  verMajor;     // = 1
  uint8_t  verMinor;     // = 0
  uint16_t headerSize;   // sizeof(HeaderV1)

  // 0x08
  uint32_t flags;        // bitmask of FontFlags

  // ---- Font metrics (match EpdFontData semantics) ----
  // 0x0C
  uint8_t  advanceY;     // newline distance (pixels)
  int16_t  ascender;     // pixels
  int16_t  descender;    // pixels
  uint8_t  reserved0;    // padding / future use

  // ---- Table counts ----
  // 0x12
  uint32_t glyphCount;
  uint32_t intervalCount;
  uint32_t groupCount;

  // Kerning / ligatures counts
  uint16_t kernLeftEntryCount;
  uint16_t kernRightEntryCount;
  uint8_t  kernLeftClassCount;
  uint8_t  kernRightClassCount;
  uint16_t reserved1;

  uint32_t ligaturePairCount;

  // ---- Section offsets + sizes ----
  // Each section is optional; if size==0 then offset must be 0.
  // Bitmap section is mandatory for any usable font.
  uint32_t bitmapOffset;
  uint32_t bitmapSize;

  uint32_t glyphTableOffset;
  uint32_t glyphTableSize;

  uint32_t intervalTableOffset;
  uint32_t intervalTableSize;

  uint32_t groupTableOffset;
  uint32_t groupTableSize;

  uint32_t glyphToGroupOffset;
  uint32_t glyphToGroupSize;

  uint32_t kernLeftOffset;
  uint32_t kernLeftSize;

  uint32_t kernRightOffset;
  uint32_t kernRightSize;

  uint32_t kernMatrixOffset;
  uint32_t kernMatrixSize;

  uint32_t ligaturePairsOffset;
  uint32_t ligaturePairsSize;

  // ---- Future expansion ----
  uint32_t reserved2[8];
};

// At the end of HeaderV1:

static constexpr size_t kHeaderV1Size = 146;
static_assert(sizeof(HeaderV1) == kHeaderV1Size, "HeaderV1 size mismatch (packing/alignment issue)");
static_assert(offsetof(HeaderV1, flags) == 0x08, "HeaderV1 layout mismatch: flags offset");
static_assert(offsetof(HeaderV1, glyphCount) == 0x12, "HeaderV1 layout mismatch: glyphCount offset");
static_assert(offsetof(HeaderV1, bitmapOffset) == 0x2A, "HeaderV1 layout mismatch: bitmapOffset offset");

// ---- Serialized records (on disk) ----

// Serialized EpdGlyph (matches fields in EpdFontData.h, but fixed packing on disk)
struct __attribute__((packed)) GlyphRecV1 {
  uint8_t  width;
  uint8_t  height;
  uint16_t advanceX;     // 12.4 fixed-point
  int16_t  left;
  int16_t  top;
  uint16_t dataLength;   // packed glyph data length (bytes)
  uint32_t dataOffset;   // if uncompressed: offset into packed bitmap stream
                         // if compressed: within-group packed offset is NOT used by runtime;
                         // runtime uses byte-aligned data + computed aligned offsets
};

// Serialized EpdFontGroup
struct __attribute__((packed)) GroupRecV1 {
  uint32_t compressedOffset;   // relative to bitmap section start
  uint32_t compressedSize;     // bytes
  uint32_t uncompressedSize;   // bytes (byte-aligned payload size after inflate)
  uint16_t glyphCount;
  uint32_t firstGlyphIndex;
};

// Serialized EpdUnicodeInterval
struct __attribute__((packed)) IntervalRecV1 {
  uint32_t first;
  uint32_t last;
  uint32_t offset; // first glyph index for this interval
};

// Serialized EpdKernClassEntry: exactly 3 bytes on disk
struct __attribute__((packed)) KernClassRecV1 {
  uint16_t codepoint; // only BMP (<= 0xFFFF) supported by class tables
  uint8_t  classId;   // 1-based; 0 means "no kerning class"
};

// Serialized EpdLigaturePair: 8 bytes on disk
struct __attribute__((packed)) LigaturePairRecV1 {
  uint32_t pair;       // left<<16 | right
  uint32_t ligatureCp; // replacement glyph codepoint
};

// ---- Minimal helpers (header-only) ----

inline bool hasFlag(const HeaderV1& h, uint32_t f) {
  return (h.flags & f) != 0;
}

inline bool sectionOk(uint32_t off, uint32_t size) {
  return (size == 0 && off == 0) || (size > 0 && off > 0);
}

inline bool validateHeaderBasic(const HeaderV1& h) {
  if (h.magic != kMagic) return false;
  if (h.verMajor != kVersionMajor) return false;
  if (h.headerSize < sizeof(HeaderV1)) return false;

  // bitmap must exist
  if (!sectionOk(h.bitmapOffset, h.bitmapSize)) return false;
  if (h.bitmapSize == 0) return false;

  // required core tables
  if (!sectionOk(h.glyphTableOffset, h.glyphTableSize)) return false;
  if (!sectionOk(h.intervalTableOffset, h.intervalTableSize)) return false;

  // If groups enabled, group table must exist
  if (hasFlag(h, FLAG_HAS_GROUPS)) {
    if (!sectionOk(h.groupTableOffset, h.groupTableSize)) return false;
    if (h.groupCount == 0) return false;
  } else {
    // If no groups, group section must be empty
    if (h.groupCount != 0) return false;
  }

  // glyphToGroup is optional but if present must have the flag
  const bool g2gPresent = (h.glyphToGroupSize > 0);
  if (g2gPresent && !hasFlag(h, FLAG_HAS_GLYPH2GROUP)) return false;
  if (!g2gPresent && hasFlag(h, FLAG_HAS_GLYPH2GROUP)) {
    // allow loader to treat as absent; but keep strict here
    return false;
  }

  // Kerning sections: all-or-nothing
  const bool kernPresent = (h.kernMatrixSize > 0 ||
                            h.kernLeftSize > 0 ||
                            h.kernRightSize > 0);
  if (kernPresent && !hasFlag(h, FLAG_HAS_KERNING)) return false;
  if (!kernPresent && hasFlag(h, FLAG_HAS_KERNING)) return false;

  // Ligatures
  const bool ligaPresent = (h.ligaturePairsSize > 0);
  if (ligaPresent && !hasFlag(h, FLAG_HAS_LIGATURES)) return false;
  if (!ligaPresent && hasFlag(h, FLAG_HAS_LIGATURES)) return false;

  return true;
}

} // namespace cpfnt