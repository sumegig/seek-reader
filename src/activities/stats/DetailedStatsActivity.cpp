#include "DetailedStatsActivity.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>

#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"  // for GUI macro and drawButtonHints
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

DetailedStatsActivity::DetailedStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t bookIndex)
    : Activity("DetailedStats", renderer, mappedInput), _bookIndex(bookIndex) {}

void DetailedStatsActivity::onEnter() {
  Activity::onEnter();
  activityManager.requestUpdateAndWait();
}

void DetailedStatsActivity::onExit() { Activity::onExit(); }

void DetailedStatsActivity::loop() {
  // We can only go back to the list from this view
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }
}

void DetailedStatsActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  renderDetailedGrid();

  // Bottom button bar (Only Back button is functional)
  // fixed: using a GUI macro
  GUI.drawButtonHints(renderer, tr(STR_BACK), "", "", "");

  renderer.displayBuffer();
}

void DetailedStatsActivity::renderDetailedGrid() const {
  const auto& book = StatsManager.getBook(_bookIndex);
  const auto& global = StatsManager.getGlobal();
  char buf[64];

  // Screen dimensions: 480x800 Portrait
  const int screenW = 480;
  const int midY = 280;
  const int botY = 450;
  const int gridBottom = 640;

  // -- 1ST SECTION: Top Left - Cover --
  // Adjusted for 480px width: Cover takes roughly 40% of width
  const int coverPad = 15;
  const int coverW = 180;
  const int coverH = 240;
  const int coverX = coverPad;
  const int coverY = coverPad;

  // Using 226px height thumb as it fits perfectly here
  std::string thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), 226);
  if (Storage.exists(thumbPath.c_str())) {
    FsFile f;
    if (Storage.openFileForRead("STATS", thumbPath.c_str(), f)) {
      Bitmap bmp(f, false);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bmp, coverX, coverY, coverW, coverH);
      }
      f.close();
    }
  } else {
    drawCoverPlaceholder(coverX, coverY, coverW, coverH);
  }

  // -- 2ND SECTION: Top Right - Book info --
  const int textX = coverX + coverW + 15;
  int textY = coverY + 10;

  // --- Optimized 3-Line title logic for Detailed View ---
  // Increased BREAK_AT to 19 to utilize the full 480px width (approx 255px for text)
  static constexpr size_t BREAK_AT = 19;
  size_t titleLen = strlen(book.title);

  if (titleLen > BREAK_AT) {
    // Line 1: Calculate first split
    char line1[32];
    size_t split1 = BREAK_AT;
    for (size_t i = BREAK_AT; i > 5; --i) {
      if (book.title[i] == ' ' || book.title[i] == ':' || book.title[i] == '|') {
        split1 = i + 1;
        break;
      }
    }
    snprintf(line1, sizeof(line1), "%.*s", (int)split1, book.title);
    renderer.drawText(UI_12_FONT_ID, textX, textY, line1, true, EpdFontFamily::BOLD);

    const char* remaining1 = book.title + split1;
    size_t rem1Len = strlen(remaining1);

    if (rem1Len > BREAK_AT) {
      // Line 2: Calculate second split from what remains
      char line2[32];
      size_t split2 = BREAK_AT;
      for (size_t i = BREAK_AT; i > 5; --i) {
        if (remaining1[i] == ' ' || remaining1[i] == ':' || remaining1[i] == '|') {
          split2 = i + 1;
          break;
        }
      }
      snprintf(line2, sizeof(line2), "%.*s", (int)split2, remaining1);
      renderer.drawText(UI_12_FONT_ID, textX, textY + 32, line2, true, EpdFontFamily::BOLD);

      // Line 3: Final segment (with safe truncation for extremely long titles)
      const char* remaining2 = remaining1 + split2;
      if (strlen(remaining2) > 22) {
        char line3Trunc[32];
        strncpy(line3Trunc, remaining2, 19);
        line3Trunc[19] = '\0';
        strcat(line3Trunc, "...");
        renderer.drawText(UI_12_FONT_ID, textX, textY + 64, line3Trunc, true, EpdFontFamily::BOLD);
      } else {
        renderer.drawText(UI_12_FONT_ID, textX, textY + 64, remaining2, true, EpdFontFamily::BOLD);
      }
      // Metadata shifted down to accommodate 3 title lines
      textY += 105;
    } else {
      // Only 2 lines needed (second line is shorter than BREAK_AT)
      renderer.drawText(UI_12_FONT_ID, textX, textY + 32, remaining1, true, EpdFontFamily::BOLD);
      textY += 85;
    }
  } else {
    // Simple 1-line title
    renderer.drawText(UI_12_FONT_ID, textX, textY, book.title, true, EpdFontFamily::BOLD);
    textY += 70;
  }

  // --- Metadata Rendering (Author, Time, Sessions) ---
  // Author
  renderer.drawText(UI_12_FONT_ID, textX, textY, book.author, true, EpdFontFamily::REGULAR);
  textY += 60;

  // Time spent
  const uint32_t hours = book.totalReadingMs / 3600000;
  const uint32_t mins = (book.totalReadingMs % 3600000) / 60000;
  snprintf(buf, sizeof(buf), "%uh %um", hours, mins);
  renderer.drawText(UI_12_FONT_ID, textX, textY, buf, true);
  textY += 30;

  // Sessions
  snprintf(buf, sizeof(buf), "Sessions: %u", book.sessionCount);
  renderer.drawText(UI_12_FONT_ID, textX, textY, buf, true);

  // -- Draw Horizontal Grid Lines --
  renderer.drawLine(0, midY, screenW, midY, 4, true);
  renderer.drawLine(0, botY, screenW, botY, 4, true);
  renderer.drawLine(0, gridBottom - 20, screenW, gridBottom - 20, 4, true);

  // Vertical divider
  renderer.drawLine(screenW / 2, midY + 35, screenW / 2, botY, 3, true);
  renderer.drawLine(screenW / 2, botY + 35, screenW / 2, gridBottom - 20, 3, true);

  // -- Section Headers (Visual Separation) --
  // "This Book" header placed in center just below the first horizontal line
  renderer.drawText(UI_10_FONT_ID, 197, midY + 8, "This Book", true, EpdFontFamily::BOLD);

  // "All Time" header placed in center just below the second horizontal line
  renderer.drawText(UI_10_FONT_ID, 202, botY + 8, " All Time", true, EpdFontFamily::BOLD);

  // -- Floating Point Calculations for Precision --
  const float totalMinsFloat = static_cast<float>(book.totalReadingMs) / 60000.0f;
  float avgMinSess = (book.sessionCount > 0) ? (totalMinsFloat / static_cast<float>(book.sessionCount)) : 0.0f;

  // Avg Pages / Min (2 decimal places)
  float avgPagesMin = (totalMinsFloat > 0.05f) ? (static_cast<float>(book.totalPagesRead) / totalMinsFloat) : 0.0f;

  // -- Render Bottom Grid with Precision --

  // Row 1: Avg Min/Sess | Avg Pages/Min
  snprintf(buf, sizeof(buf), "%.1f", avgMinSess);
  renderer.drawText(UI_12_FONT_ID, 100, botY - 100, buf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 30, botY - 60, "  Avg min/session", true);

  snprintf(buf, sizeof(buf), "%.2f", avgPagesMin);
  renderer.drawText(UI_12_FONT_ID, 340, botY - 100, buf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 270, botY - 60, "   Avg pages/min", true);

  // -- Section: Book Milestones (Bottom Grid) --
  const int botYVal = 520;
  const int botYLabel = 560;

  // 1. Column: Last Session duration
  char lastSessBuf[32];
  snprintf(lastSessBuf, sizeof(lastSessBuf), "%u min", static_cast<unsigned>(book.lastSessionMs / 60000));
  renderer.drawText(UI_12_FONT_ID, 90, botYVal, lastSessBuf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 20, botYLabel, "Last session this book", true);

  // 2. Column: Total Reading Time for this specific book
  char globalTotalBuf[32];
  uint32_t bh = global.totalReadingMs / 3600000;
  uint32_t bm = (global.totalReadingMs % 3600000) / 60000;
  snprintf(globalTotalBuf, sizeof(globalTotalBuf), "%uh %02um", bh, bm);
  renderer.drawText(UI_12_FONT_ID, 310, botYVal, globalTotalBuf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 290, botYLabel, "Total time read", true);
}

void DetailedStatsActivity::drawCoverPlaceholder(int x, int y, int w, int h) const {
  static constexpr const char* PLACEHOLDER_PATH = "/.crosspoint/system/BasicCover.bmp";
  if (Storage.exists(PLACEHOLDER_PATH)) {
    FsFile f;
    if (Storage.openFileForRead("STATS", PLACEHOLDER_PATH, f)) {
      Bitmap bmp(f, false);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawRect(x, y, w, h, 2, true);
        renderer.drawBitmap(bmp, x + 2, y + 2, w - 4, h - 4);
        f.close();
        return;
      }
      f.close();
    }
  }
  renderer.drawRoundedRect(x, y, w, h, 1, 4, true);
}