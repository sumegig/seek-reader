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
  // const int botY2 = 490; // second horizontal line
  const int gridBottom = 640;

  // -- 1ST SECTION: Top Left - Cover --
  // Adjusted for 480px width: Cover takes roughly 40% of width
  const int coverPad = 15;
  const int coverW = 180;
  const int coverH = 240;  // 3:4 aspect ratio
  const int coverX = coverPad;
  const int coverY = coverPad;

  bool coverFound = false;
  // Using 226px height thumb as it fits perfectly here
  std::string thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), 226);
  if (Storage.exists(thumbPath.c_str())) {
    FsFile f;
    if (Storage.openFileForRead("STATS", thumbPath.c_str(), f)) {
      Bitmap bmp(f, false);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bmp, coverX, coverY, coverW, coverH);
        coverFound = true;
      }
      f.close();
    }
  }

  if (!coverFound) {
    drawCoverPlaceholder(coverX, coverY, coverW, coverH);
  }

  // -- 2ND SECTION: Top Right - Book info --
  const int textX = coverX + coverW + 15;
  int textY = coverY + 10;

  // Title - BOLD (Using UI_12_FONT_ID as UI_14/16 are not loaded)
  renderer.drawText(UI_12_FONT_ID, textX, textY, book.title, true, EpdFontFamily::BOLD);
  textY += 40;

  // Author
  renderer.drawText(UI_12_FONT_ID, textX, textY, book.author, true, EpdFontFamily::REGULAR);
  textY += 60;

  // Time spent
  const uint32_t hours = book.totalReadingMs / 3600000;
  const uint32_t mins = (book.totalReadingMs % 3600000) / 60000;
  snprintf(buf, sizeof(buf), "%uh %um", hours, mins);
  renderer.drawText(UI_12_FONT_ID, textX, textY, buf, true);
  textY += 35;

  // Sessions
  snprintf(buf, sizeof(buf), "Sessions: %u", book.sessionCount);
  renderer.drawText(UI_12_FONT_ID, textX, textY, buf, true);

  // -- Draw Horizontal Grid Lines --
  renderer.drawLine(0, midY, screenW, midY, 4, true);
  renderer.drawLine(0, botY, screenW, botY, 4, true);
  // renderer.drawLine(0, botY2, screenW, botY2, 4, true);  // Second line for separating
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
  const float totalMins = static_cast<float>(book.totalReadingMs) / 60000.0f;

  // Avg Min / Session (1 decimal place)
  float avgMinSess = 0.0f;
  if (book.sessionCount > 0) {
    avgMinSess = totalMins / static_cast<float>(book.sessionCount);
  }

  // Avg Pages / Min (2 decimal places)
  float avgPagesMin = 0.0f;
  if (totalMins > 0.05f) {  // Avoid division by near-zero
    avgPagesMin = static_cast<float>(book.totalPagesRead) / totalMins;
  }

  // Global Hours (1 decimal place)
  const float globalHours = static_cast<float>(global.totalReadingMs) / 3600000.0f;

  // -- Render Bottom Grid with Precision --

  // Row 1: Avg Min/Sess | Avg Pages/Min
  snprintf(buf, sizeof(buf), "%.1f", avgMinSess);  // 1 decimal point
  renderer.drawText(UI_12_FONT_ID, 100, botY - 100, buf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 30, botY - 60, "  Avg min/session", true);

  snprintf(buf, sizeof(buf), "%.2f", avgPagesMin);  // 2 decimal points
  renderer.drawText(UI_12_FONT_ID, 340, botY - 100, buf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 270, botY - 60, "   Avg pages/min", true);

  // -- Section: Book Milestones (Bottom Grid) --
  const int botYVal = 520;    // Adjusted Y position for stats values
  const int botYLabel = 560;  // Adjusted Y position for labels

  // 1. Column: Last Session duration
  char lastSessBuf[32];
  uint32_t lsMins = book.lastSessionMs / 60000;
  snprintf(lastSessBuf, sizeof(lastSessBuf), "%u min", static_cast<unsigned>(lsMins));
  renderer.drawText(UI_12_FONT_ID, 90, botYVal, lastSessBuf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, 20, botYLabel, "Last session this book", true);

  // 2. Column: Total Reading Time for this specific book
  char globalTotalBuf[32];
  uint32_t bh = global.totalReadingMs / 3600000;
  uint32_t bm = (global.totalReadingMs % 3600000) / 60000;
  snprintf(globalTotalBuf, sizeof(globalTotalBuf), "%uh %02um", static_cast<unsigned>(bh), static_cast<unsigned>(bm));
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