#pragma once

#include <cstdint>

#include <Logging.h>

#include "CrossPointSettings.h"
#include "FontCacheManager.h"
#include "GfxRenderer.h"
#include "fontIds.h"

#include "fonts/CustomFontsIndex.h"
#include "fonts/CustomFontManager.h"

// CustomFontRuntime
// - Owns CustomFontsIndex + CustomFontManager
// - Loads custom_fonts.json index (once)
// - Ensures the active custom font (slot + current reader size) is loaded
//   and inserted into renderer under CUSTOM_FONT_ID.
class CustomFontRuntime {
 public:
  bool begin() {
    const bool ok = index_.loadOrCreate();
    if (!ok) {
      LOG_ERR("CFR", "Failed to loadOrCreate custom fonts index");
    }
    return ok;
  }

  // Call before rendering any page that might use CUSTOM_FONT_ID.
  // Returns true if custom font is active+loaded, false if not active or load failed.
  bool ensureLoadedForCurrentSettings(GfxRenderer& renderer, FontCacheManager& fontCacheManager) {
    if (SETTINGS.customFontSlot == 0) {
      // Custom disabled; nothing to do.
      return false;
    }

    const uint8_t slot = SETTINGS.customFontSlot;
    const uint8_t sizePx = mapReaderFontSizeToPx(SETTINGS.fontSize);

    // No-op if already active with same parameters
    if (mgr_.isActive() && mgr_.slotId() == slot && mgr_.sizePx() == sizePx) {
      return true;
    }

    LOG_DBG("CFR", "Loading custom font slot=%u size=%u for CUSTOM_FONT_ID=%d", slot, sizePx, CUSTOM_FONT_ID);

    // Any font swap should clear decompressor caches
    fontCacheManager.clearCache();

    if (!mgr_.activate(slot, sizePx)) {
      LOG_ERR("CFR", "activate(slot=%u,size=%u) failed; custom font disabled for this render", slot, sizePx);
      return false;
    }

    // Overwrite or insert CUSTOM_FONT_ID -> this custom family
    renderer.insertFont(CUSTOM_FONT_ID, mgr_.family());
    return true;
  }

  // Optional: expose index for UI/web later
  CustomFontsIndex& index() { return index_; }
  const CustomFontsIndex& index() const { return index_; }

 private:
  CustomFontsIndex index_;
  CustomFontManager mgr_{index_};

  static uint8_t mapReaderFontSizeToPx(uint8_t fontSizeEnum) {
    // Mirrors your built-in mapping (Bookerly/Notosans): 10/12/14/16/18 
    switch (fontSizeEnum) {
      case CrossPointSettings::EXTRA_SMALL:
        return 10;
      case CrossPointSettings::SMALL:
        return 12;
      case CrossPointSettings::LARGE:
        return 16;
      case CrossPointSettings::EXTRA_LARGE:
        return 18;
      case CrossPointSettings::MEDIUM:
      default:
        return 14;
    }
  }
};
extern CustomFontRuntime CUSTOM_FONT_RUNTIME;