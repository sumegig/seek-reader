#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// Project headers
#include "CustomFontBinFormat.h"
#include "EpdFontData.h"

// CustomFontBinLoader
// Loads a CPFN v1 *compressed 2-bit* font from SD into heap-allocated runtime tables.
//
// Output is an owning handle (LoadedFont) that keeps all memory alive.
// Use loaded->fontData() anywhere an EpdFontData* is expected.
//
class CustomFontBinLoader {
 public:
  struct LoadedFont {
    // Runtime-owned allocations backing EpdFontData pointers
    uint8_t* bitmap = nullptr;

    EpdGlyph* glyph = nullptr;
    EpdUnicodeInterval* intervals = nullptr;
    EpdFontGroup* groups = nullptr;

    uint16_t* glyphToGroup = nullptr;  // optional

    EpdKernClassEntry* kernLeft = nullptr;   // optional
    EpdKernClassEntry* kernRight = nullptr;  // optional
    int8_t* kernMatrix = nullptr;            // optional

    EpdLigaturePair* ligaturePairs = nullptr;  // optional

    // The struct consumed by EpdFont / GfxRenderer / FontDecompressor
    EpdFontData data{};

    // Keep these for debugging / sanity checks
    cpfnt::HeaderV1 header{};

    ~LoadedFont() { freeAll(); }

    LoadedFont() = default;
    LoadedFont(const LoadedFont&) = delete;
    LoadedFont& operator=(const LoadedFont&) = delete;

    LoadedFont(LoadedFont&& other) noexcept { *this = std::move(other); }
    LoadedFont& operator=(LoadedFont&& other) noexcept {
      if (this == &other) return *this;
      freeAll();

      bitmap = other.bitmap;
      glyph = other.glyph;
      intervals = other.intervals;
      groups = other.groups;
      glyphToGroup = other.glyphToGroup;
      kernLeft = other.kernLeft;
      kernRight = other.kernRight;
      kernMatrix = other.kernMatrix;
      ligaturePairs = other.ligaturePairs;
      data = other.data;
      header = other.header;

      other.bitmap = nullptr;
      other.glyph = nullptr;
      other.intervals = nullptr;
      other.groups = nullptr;
      other.glyphToGroup = nullptr;
      other.kernLeft = nullptr;
      other.kernRight = nullptr;
      other.kernMatrix = nullptr;
      other.ligaturePairs = nullptr;
      other.data = {};
      other.header = {};

      return *this;
    }

    const EpdFontData* fontData() const { return &data; }

   private:
    void freeAll() {
      free(bitmap);
      free(glyph);
      free(intervals);
      free(groups);
      free(glyphToGroup);
      free(kernLeft);
      free(kernRight);
      free(kernMatrix);
      free(ligaturePairs);

      bitmap = nullptr;
      glyph = nullptr;
      intervals = nullptr;
      groups = nullptr;
      glyphToGroup = nullptr;
      kernLeft = nullptr;
      kernRight = nullptr;
      kernMatrix = nullptr;
      ligaturePairs = nullptr;
      data = {};
      header = {};
    }
  };

  // Load a CPFN font from the SD card.
  // Returns nullptr on failure.
  static std::unique_ptr<LoadedFont> loadFromSdPath(const char* path) {
    if (!path || !*path) {
      LOG_ERR("CFBL", "Invalid path");
      return nullptr;
    }

    FsFile f;
    if (!Storage.openFileForRead("CFBL", path, f)) {
      LOG_ERR("CFBL", "Failed to open for read: %s", path);
      return nullptr;
    }

    auto loaded = std::make_unique<LoadedFont>();

    // ---- Read header ----
    cpfnt::HeaderV1 h{};
    if (!readExact(f, 0, &h, sizeof(h))) {
      LOG_ERR("CFBL", "Failed to read header: %s", path);
      f.close();
      return nullptr;
    }

    // Enforce compressed-only + 2-bit (required by FontDecompressor compact path)
    if (!cpfnt::validateHeaderBasic(h)) {
      LOG_ERR("CFBL", "Header validation failed: %s", path);
      f.close();
      return nullptr;
    }
    if (!cpfnt::hasFlag(h, cpfnt::FLAG_HAS_GROUPS) || !cpfnt::hasFlag(h, cpfnt::FLAG_IS_2BIT)) {
      LOG_ERR("CFBL", "Font is not compressed 2-bit (required): %s", path);
      f.close();
      return nullptr;
    }
    if (h.groupCount == 0) {
      LOG_ERR("CFBL", "Compressed font has groupCount=0: %s", path);
      f.close();
      return nullptr;
    }
    if (h.glyphCount == 0 || h.intervalCount == 0) {
      LOG_ERR("CFBL", "Invalid counts glyphCount=%lu intervalCount=%lu: %s",
              (unsigned long)h.glyphCount, (unsigned long)h.intervalCount, path);
      f.close();
      return nullptr;
    }
    if (h.glyphCount > 200000) {  // sanity cap
      LOG_ERR("CFBL", "glyphCount too large: %lu", (unsigned long)h.glyphCount);
      f.close();
      return nullptr;
    }
    if (h.groupCount > 65535) {  // EpdFontData stores groupCount as uint16_t
      LOG_ERR("CFBL", "groupCount too large for runtime: %lu", (unsigned long)h.groupCount);
      f.close();
      return nullptr;
    }

    loaded->header = h;

    // ---- Validate section sizes against counts ----
    const uint32_t wantGlyphTableSize = h.glyphCount * (uint32_t)sizeof(cpfnt::GlyphRecV1);
    const uint32_t wantIntervalTableSize = h.intervalCount * (uint32_t)sizeof(cpfnt::IntervalRecV1);
    const uint32_t wantGroupTableSize = h.groupCount * (uint32_t)sizeof(cpfnt::GroupRecV1);

    if (h.glyphTableSize != wantGlyphTableSize) {
      LOG_ERR("CFBL", "glyphTableSize mismatch: have=%lu want=%lu", (unsigned long)h.glyphTableSize,
              (unsigned long)wantGlyphTableSize);
      f.close();
      return nullptr;
    }
    if (h.intervalTableSize != wantIntervalTableSize) {
      LOG_ERR("CFBL", "intervalTableSize mismatch: have=%lu want=%lu", (unsigned long)h.intervalTableSize,
              (unsigned long)wantIntervalTableSize);
      f.close();
      return nullptr;
    }
    if (h.groupTableSize != wantGroupTableSize) {
      LOG_ERR("CFBL", "groupTableSize mismatch: have=%lu want=%lu", (unsigned long)h.groupTableSize,
              (unsigned long)wantGroupTableSize);
      f.close();
      return nullptr;
    }

    if (cpfnt::hasFlag(h, cpfnt::FLAG_HAS_GLYPH2GROUP)) {
      const uint32_t want = h.glyphCount * (uint32_t)sizeof(uint16_t);
      if (h.glyphToGroupSize != want) {
        LOG_ERR("CFBL", "glyphToGroupSize mismatch: have=%lu want=%lu", (unsigned long)h.glyphToGroupSize,
                (unsigned long)want);
        f.close();
        return nullptr;
      }
    } else {
      if (h.glyphToGroupSize != 0 || h.glyphToGroupOffset != 0) {
        LOG_ERR("CFBL", "glyphToGroup present but flag missing");
        f.close();
        return nullptr;
      }
    }

    if (cpfnt::hasFlag(h, cpfnt::FLAG_HAS_KERNING)) {
      // kern classes arrays are packed 3 bytes entries in EpdFontData.h
      const uint32_t wantLeft = h.kernLeftEntryCount * (uint32_t)sizeof(cpfnt::KernClassRecV1);
      const uint32_t wantRight = h.kernRightEntryCount * (uint32_t)sizeof(cpfnt::KernClassRecV1);
      const uint32_t wantMatrix = (uint32_t)h.kernLeftClassCount * (uint32_t)h.kernRightClassCount;

      if (h.kernLeftSize != wantLeft || h.kernRightSize != wantRight || h.kernMatrixSize != wantMatrix) {
        LOG_ERR("CFBL", "Kerning section size mismatch (left/right/matrix)");
        f.close();
        return nullptr;
      }
    } else {
      if (h.kernLeftSize || h.kernRightSize || h.kernMatrixSize) {
        LOG_ERR("CFBL", "Kerning sections exist but FLAG_HAS_KERNING not set");
        f.close();
        return nullptr;
      }
    }

    if (cpfnt::hasFlag(h, cpfnt::FLAG_HAS_LIGATURES)) {
      const uint32_t want = h.ligaturePairCount * (uint32_t)sizeof(cpfnt::LigaturePairRecV1);
      if (h.ligaturePairsSize != want) {
        LOG_ERR("CFBL", "ligaturePairsSize mismatch: have=%lu want=%lu", (unsigned long)h.ligaturePairsSize,
                (unsigned long)want);
        f.close();
        return nullptr;
      }
    } else {
      if (h.ligaturePairsSize) {
        LOG_ERR("CFBL", "Ligature section exists but flag missing");
        f.close();
        return nullptr;
      }
    }

    // ---- Load bitmap blob (compressed DEFLATE streams concatenated) ----
    loaded->bitmap = static_cast<uint8_t*>(malloc(h.bitmapSize));
    if (!loaded->bitmap) {
      LOG_ERR("CFBL", "Failed to allocate bitmap %lu bytes", (unsigned long)h.bitmapSize);
      f.close();
      return nullptr;
    }
    if (!readExact(f, h.bitmapOffset, loaded->bitmap, h.bitmapSize)) {
      LOG_ERR("CFBL", "Failed to read bitmap blob");
      f.close();
      return nullptr;
    }

    // ---- Load tables into runtime structs ----

    // Glyphs
    loaded->glyph = static_cast<EpdGlyph*>(malloc(h.glyphCount * sizeof(EpdGlyph)));
    if (!loaded->glyph) {
      LOG_ERR("CFBL", "Failed to allocate glyph table");
      f.close();
      return nullptr;
    }
    {
      std::vector<cpfnt::GlyphRecV1> glyphRecs;
      glyphRecs.resize(h.glyphCount);
      if (!readExact(f, h.glyphTableOffset, glyphRecs.data(), h.glyphTableSize)) {
        LOG_ERR("CFBL", "Failed to read glyph table");
        f.close();
        return nullptr;
      }
      for (uint32_t i = 0; i < h.glyphCount; i++) {
        const auto& r = glyphRecs[i];
        loaded->glyph[i].width = r.width;
        loaded->glyph[i].height = r.height;
        loaded->glyph[i].advanceX = r.advanceX;
        loaded->glyph[i].left = r.left;
        loaded->glyph[i].top = r.top;
        loaded->glyph[i].dataLength = r.dataLength;
        loaded->glyph[i].dataOffset = r.dataOffset;
      }
    }

    // Intervals
    loaded->intervals = static_cast<EpdUnicodeInterval*>(malloc(h.intervalCount * sizeof(EpdUnicodeInterval)));
    if (!loaded->intervals) {
      LOG_ERR("CFBL", "Failed to allocate interval table");
      f.close();
      return nullptr;
    }
    {
      std::vector<cpfnt::IntervalRecV1> intervalRecs;
      intervalRecs.resize(h.intervalCount);
      if (!readExact(f, h.intervalTableOffset, intervalRecs.data(), h.intervalTableSize)) {
        LOG_ERR("CFBL", "Failed to read interval table");
        f.close();
        return nullptr;
      }
      for (uint32_t i = 0; i < h.intervalCount; i++) {
        loaded->intervals[i].first = intervalRecs[i].first;
        loaded->intervals[i].last = intervalRecs[i].last;
        loaded->intervals[i].offset = intervalRecs[i].offset;
      }

      // Sanity: glyphCount must cover the last interval's range
      const auto& last = loaded->intervals[h.intervalCount - 1];
      const uint64_t requiredGlyphs = (uint64_t)last.offset + (uint64_t)(last.last - last.first + 1);
      if (requiredGlyphs != (uint64_t)h.glyphCount) {
        LOG_ERR("CFBL", "glyphCount mismatch vs intervals: header=%lu intervals_imply=%llu",
                (unsigned long)h.glyphCount, (unsigned long long)requiredGlyphs);
        f.close();
        return nullptr;
      }
    }

    // Groups
    loaded->groups = static_cast<EpdFontGroup*>(malloc(h.groupCount * sizeof(EpdFontGroup)));
    if (!loaded->groups) {
      LOG_ERR("CFBL", "Failed to allocate group table");
      f.close();
      return nullptr;
    }
    {
      std::vector<cpfnt::GroupRecV1> groupRecs;
      groupRecs.resize(h.groupCount);
      if (!readExact(f, h.groupTableOffset, groupRecs.data(), h.groupTableSize)) {
        LOG_ERR("CFBL", "Failed to read group table");
        f.close();
        return nullptr;
      }
      for (uint32_t i = 0; i < h.groupCount; i++) {
        loaded->groups[i].compressedOffset = groupRecs[i].compressedOffset;
        loaded->groups[i].compressedSize = groupRecs[i].compressedSize;
        loaded->groups[i].uncompressedSize = groupRecs[i].uncompressedSize;
        loaded->groups[i].glyphCount = groupRecs[i].glyphCount;
        loaded->groups[i].firstGlyphIndex = groupRecs[i].firstGlyphIndex;

        // Minimal sanity: compressed block must lie within bitmap blob
        const uint64_t start = (uint64_t)loaded->groups[i].compressedOffset;
        const uint64_t end = start + (uint64_t)loaded->groups[i].compressedSize;
        if (end > h.bitmapSize) {
          LOG_ERR("CFBL", "Group %lu compressed range out of bitmap bounds (end=%llu bitmapSize=%lu)",
                  (unsigned long)i, (unsigned long long)end, (unsigned long)h.bitmapSize);
          f.close();
          return nullptr;
        }
      }
    }

    // glyphToGroup (optional)
    if (cpfnt::hasFlag(h, cpfnt::FLAG_HAS_GLYPH2GROUP)) {
      loaded->glyphToGroup = static_cast<uint16_t*>(malloc(h.glyphCount * sizeof(uint16_t)));
      if (!loaded->glyphToGroup) {
        LOG_ERR("CFBL", "Failed to allocate glyphToGroup");
        f.close();
        return nullptr;
      }
      if (!readExact(f, h.glyphToGroupOffset, loaded->glyphToGroup, h.glyphToGroupSize)) {
        LOG_ERR("CFBL", "Failed to read glyphToGroup");
        f.close();
        return nullptr;
      }
      // Sanity: group indices should be < groupCount
      for (uint32_t i = 0; i < h.glyphCount; i++) {
        if (loaded->glyphToGroup[i] >= h.groupCount) {
          LOG_ERR("CFBL", "glyphToGroup[%lu]=%u out of range (groupCount=%lu)",
                  (unsigned long)i, loaded->glyphToGroup[i], (unsigned long)h.groupCount);
          f.close();
          return nullptr;
        }
      }
    }

    // Kerning (optional)
    if (cpfnt::hasFlag(h, cpfnt::FLAG_HAS_KERNING)) {
      if (h.kernLeftEntryCount > 0) {
        loaded->kernLeft = static_cast<EpdKernClassEntry*>(malloc(h.kernLeftEntryCount * sizeof(EpdKernClassEntry)));
        if (!loaded->kernLeft) {
          LOG_ERR("CFBL", "Failed to alloc kernLeft");
          f.close();
          return nullptr;
        }
        if (!readExact(f, h.kernLeftOffset, loaded->kernLeft, h.kernLeftSize)) {
          LOG_ERR("CFBL", "Failed to read kernLeft");
          f.close();
          return nullptr;
        }
      }

      if (h.kernRightEntryCount > 0) {
        loaded->kernRight = static_cast<EpdKernClassEntry*>(malloc(h.kernRightEntryCount * sizeof(EpdKernClassEntry)));
        if (!loaded->kernRight) {
          LOG_ERR("CFBL", "Failed to alloc kernRight");
          f.close();
          return nullptr;
        }
        if (!readExact(f, h.kernRightOffset, loaded->kernRight, h.kernRightSize)) {
          LOG_ERR("CFBL", "Failed to read kernRight");
          f.close();
          return nullptr;
        }
      }

      if (h.kernMatrixSize > 0) {
        loaded->kernMatrix = static_cast<int8_t*>(malloc(h.kernMatrixSize));
        if (!loaded->kernMatrix) {
          LOG_ERR("CFBL", "Failed to alloc kernMatrix");
          f.close();
          return nullptr;
        }
        if (!readExact(f, h.kernMatrixOffset, loaded->kernMatrix, h.kernMatrixSize)) {
          LOG_ERR("CFBL", "Failed to read kernMatrix");
          f.close();
          return nullptr;
        }
      }
    }

    // Ligatures (optional)
    if (cpfnt::hasFlag(h, cpfnt::FLAG_HAS_LIGATURES) && h.ligaturePairCount > 0) {
      loaded->ligaturePairs =
          static_cast<EpdLigaturePair*>(malloc(h.ligaturePairCount * sizeof(EpdLigaturePair)));
      if (!loaded->ligaturePairs) {
        LOG_ERR("CFBL", "Failed to alloc ligaturePairs");
        f.close();
        return nullptr;
      }
      if (!readExact(f, h.ligaturePairsOffset, loaded->ligaturePairs, h.ligaturePairsSize)) {
        LOG_ERR("CFBL", "Failed to read ligaturePairs");
        f.close();
        return nullptr;
      }
    }

    // ---- Populate EpdFontData ----
    loaded->data.bitmap = loaded->bitmap;
    loaded->data.glyph = loaded->glyph;
    loaded->data.intervals = loaded->intervals;
    loaded->data.intervalCount = h.intervalCount;

    loaded->data.advanceY = h.advanceY;
    loaded->data.ascender = h.ascender;
    loaded->data.descender = h.descender;
    loaded->data.is2Bit = true;

    loaded->data.groups = loaded->groups;
    loaded->data.groupCount = static_cast<uint16_t>(h.groupCount);
    loaded->data.glyphToGroup = loaded->glyphToGroup;

    loaded->data.kernLeftClasses = loaded->kernLeft;
    loaded->data.kernRightClasses = loaded->kernRight;
    loaded->data.kernMatrix = loaded->kernMatrix;
    loaded->data.kernLeftEntryCount = h.kernLeftEntryCount;
    loaded->data.kernRightEntryCount = h.kernRightEntryCount;
    loaded->data.kernLeftClassCount = h.kernLeftClassCount;
    loaded->data.kernRightClassCount = h.kernRightClassCount;

    loaded->data.ligaturePairs = loaded->ligaturePairs;
    loaded->data.ligaturePairCount = h.ligaturePairCount;

    f.close();
    LOG_DBG("CFBL", "Loaded font: %s (glyphs=%lu groups=%lu bitmap=%lu bytes)",
            path, (unsigned long)h.glyphCount, (unsigned long)h.groupCount, (unsigned long)h.bitmapSize);
    return loaded;
  }

 private:
  static bool readExact(FsFile& f, uint32_t offset, void* dst, uint32_t len) {
    if (len == 0) return true;
    if (!dst) return false;

    if (!f.seek(offset)) {
      LOG_ERR("CFBL", "seek(%lu) failed", (unsigned long)offset);
      return false;
    }

    uint8_t* out = static_cast<uint8_t*>(dst);
    uint32_t remaining = len;
    while (remaining > 0) {
      // Use modest chunk reads to avoid large internal allocations
      const uint32_t chunk = remaining > 4096 ? 4096 : remaining;
      const int r = f.read(out, chunk);
      if (r <= 0) {
        LOG_ERR("CFBL", "read failed (wanted %lu, got %d)", (unsigned long)chunk, r);
        return false;
      }
      out += static_cast<uint32_t>(r);
      remaining -= static_cast<uint32_t>(r);
    }
    return true;
  }
};