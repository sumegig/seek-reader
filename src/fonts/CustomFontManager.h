#pragma once

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>

// Your existing font types
#include "EpdFont.h"
#include "EpdFontFamily.h"

// New modules we added
#include "CustomFontBinLoader.h"
#include "CustomFontsIndex.h"

// CustomFontManager
//
// Purpose:
//   Owns the runtime-loaded custom fonts (CPFN) for ONE active selection:
//     slotId (1..3) + sizePx (10/12/14/16/18)
//
// Current behavior (minimal-invasive / reliable):
//   - Loads Regular (required)
//   - Loads Bold/Italic/BoldItalic best-effort *if the slot index says they exist*
//   - Any missing/failed style falls back to Regular
//
// Family exposure:
//   - Keeps stable EpdFont wrapper objects alive (pointers used by EpdFontFamily)
//   - Returns an EpdFontFamily by value that points at those stable wrappers
//
class CustomFontManager {
 public:
  explicit CustomFontManager(CustomFontsIndex& index) : index_(index) {}

  // Activate a slot+size. Returns true only if Regular loads successfully.
  bool activate(uint8_t slotId, uint8_t sizePx) {
    slotId_ = slotId;
    sizePx_ = sizePx;

    if (slotId_ < 1 || slotId_ > CustomFontsIndex::kSlotCount) {
      LOG_ERR("CFM", "Invalid slotId=%u", slotId_);
      clearLoaded();
      return false;
    }

    if (!index_.slotExists(slotId_)) {
      LOG_ERR("CFM", "Slot %u does not exist (inactive or no name)", slotId_);
      clearLoaded();
      return false;
    }

    // Load Regular first (required)
    regular_ = loadStyle("regular");
    if (!regular_) {
      LOG_ERR("CFM", "Failed to load Regular for slot=%u size=%u", slotId_, sizePx_);
      clearLoaded();
      return false;
    }

    // Ensure the 4 EpdFont wrapper objects exist (stable pointers for EpdFontFamily)
    ensureWrappers();

    // Point everything at Regular initially (safe fallback)
    fontRegular_->data = regular_->fontData();
    fontBold_->data = regular_->fontData();
    fontItalic_->data = regular_->fontData();
    fontBoldItalic_->data = regular_->fontData();

    // Load other styles best-effort
    const uint8_t mask = index_.slotStyleMask(slotId_) & 0x0F;

    // Bold
    if (mask & CustomFontsIndex::STYLE_BOLD) {
      bold_ = loadStyle("bold");
      if (bold_) fontBold_->data = bold_->fontData();
      else LOG_ERR("CFM", "Bold missing/failed, falling back to Regular");
    } else {
      bold_.reset();
    }

    // Italic
    if (mask & CustomFontsIndex::STYLE_ITALIC) {
      italic_ = loadStyle("italic");
      if (italic_) fontItalic_->data = italic_->fontData();
      else LOG_ERR("CFM", "Italic missing/failed, falling back to Regular");
    } else {
      italic_.reset();
    }

    // BoldItalic
    if (mask & CustomFontsIndex::STYLE_BOLD_ITALIC) {
      boldItalic_ = loadStyle("bolditalic");
      if (boldItalic_) fontBoldItalic_->data = boldItalic_->fontData();
      else LOG_ERR("CFM", "BoldItalic missing/failed, falling back to Regular");
    } else {
      boldItalic_.reset();
    }

    // Sanity check: must look like a compressed 2-bit font for FontDecompressor path
    if (!fontRegular_->data || !fontRegular_->data->groups || fontRegular_->data->groupCount == 0 ||
        !fontRegular_->data->is2Bit) {
      LOG_ERR("CFM", "Regular loaded but does not look like a compressed 2-bit font");
      clearLoaded();
      return false;
    }

    LOG_DBG("CFM", "Activated slot=%u name='%s' size=%u (styles loaded: R=%d B=%d I=%d BI=%d)",
            slotId_,
            index_.slotName(slotId_),
            sizePx_,
            regular_ != nullptr,
            bold_ != nullptr,
            italic_ != nullptr,
            boldItalic_ != nullptr);

    return true;
  }

  void deactivate() {
    slotId_ = 0;
    sizePx_ = 0;
    clearLoaded();

    // Leave wrappers alive but set to nullptr to make misuse obvious during dev.
    if (fontRegular_) fontRegular_->data = nullptr;
    if (fontBold_) fontBold_->data = nullptr;
    if (fontItalic_) fontItalic_->data = nullptr;
    if (fontBoldItalic_) fontBoldItalic_->data = nullptr;
  }

  bool isActive() const {
    return (slotId_ >= 1 && slotId_ <= CustomFontsIndex::kSlotCount && regular_ != nullptr);
  }

  uint8_t slotId() const { return slotId_; }
  uint8_t sizePx() const { return sizePx_; }

  // Family usable by GfxRenderer::insertFont(fontId, family)
  // Only valid if isActive()==true.
  EpdFontFamily family() const {
    // EpdFontFamily stores pointers; we return by value.
    return EpdFontFamily(fontRegular_.get(), fontBold_.get(), fontItalic_.get(), fontBoldItalic_.get());
  }

  // Resolve filename path for a given style string.
  // styleStr must be: "regular" | "bold" | "italic" | "bolditalic"
  String buildFontPath(const char* styleStr) const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s/custom_slot%u_%u_%s.bin",
                  CustomFontsIndex::kDir, slotId_, sizePx_, styleStr);
    return String(buf);
  }

 private:
  using LoadedFontPtr = std::unique_ptr<CustomFontBinLoader::LoadedFont>;

  CustomFontsIndex& index_;

  uint8_t slotId_ = 0;
  uint8_t sizePx_ = 0;

  // Owned loaded fonts
  LoadedFontPtr regular_;
  LoadedFontPtr bold_;
  LoadedFontPtr italic_;
  LoadedFontPtr boldItalic_;

  // Stable wrapper objects referenced by EpdFontFamily
  std::unique_ptr<EpdFont> fontRegular_;
  std::unique_ptr<EpdFont> fontBold_;
  std::unique_ptr<EpdFont> fontItalic_;
  std::unique_ptr<EpdFont> fontBoldItalic_;

  void ensureWrappers() {
    if (!fontRegular_) fontRegular_ = std::make_unique<EpdFont>(nullptr);
    if (!fontBold_) fontBold_ = std::make_unique<EpdFont>(nullptr);
    if (!fontItalic_) fontItalic_ = std::make_unique<EpdFont>(nullptr);
    if (!fontBoldItalic_) fontBoldItalic_ = std::make_unique<EpdFont>(nullptr);
  }

  void clearLoaded() {
    boldItalic_.reset();
    italic_.reset();
    bold_.reset();
    regular_.reset();
  }

  LoadedFontPtr loadStyle(const char* styleStr) const {
    const String path = buildFontPath(styleStr);

    if (!Storage.exists(path.c_str())) {
      LOG_ERR("CFM", "Missing font file: %s", path.c_str());
      return nullptr;
    }

    auto loaded = CustomFontBinLoader::loadFromSdPath(path.c_str());
    if (!loaded) {
      LOG_ERR("CFM", "Failed to load font file: %s", path.c_str());
      return nullptr;
    }

    return loaded;
  }
};