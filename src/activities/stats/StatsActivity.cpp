#include "activities/stats/StatsActivity.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DetailedStatsActivity.h"  // detailedStatistics
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

static constexpr int COVER_PAD = 6;

// -----------------------------------------------------------------------
// Static helpers
// -----------------------------------------------------------------------

/**
 * @brief Formats a duration in milliseconds into a human-readable string (e.g., "Xh Ym" or "Ym").
 */
void StatsActivity::formatDuration(char* buf, size_t bufLen, uint32_t ms) {
  const uint32_t totalMin = ms / 60000UL;
  const uint32_t hours = totalMin / 60UL;
  const uint32_t mins = totalMin % 60UL;
  if (hours > 0) {
    snprintf(buf, bufLen, "%uh %02um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
  } else {
    snprintf(buf, bufLen, "%um", static_cast<unsigned>(mins));
  }
}

/**
 * @brief Formats a progress percentage into a string (e.g., "100%").
 */
void StatsActivity::formatPercent(char* buf, size_t bufLen, uint8_t percent) {
  snprintf(buf, bufLen, "%u%%", static_cast<unsigned>(percent));
}

/**
 * @brief Returns the number of books that have not yet reached 100% completion.
 * Completed books are filtered out from the active statistics list.
 */
uint8_t StatsActivity::getVisibleBookCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < StatsManager.getBookCount(); ++i) {
    if (StatsManager.getBook(i).progressPercent < 100) {
      count++;
    }
  }
  return count;
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void StatsActivity::onEnter() {
  Activity::onEnter();
  selectedBookIndex = 0;
  activityManager.requestUpdateAndWait();
}

void StatsActivity::onExit() { Activity::onExit(); }

// -----------------------------------------------------------------------
// Input Handling
// -----------------------------------------------------------------------

void StatsActivity::loop() {
  // Return to the Home screen
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }

  const uint8_t bookCount = getVisibleBookCount();
  if (bookCount == 0) return;

  bool changed = false;

  // Navigate up in the book list
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (selectedBookIndex > 0) {
      selectedBookIndex--;
      changed = true;
    }
  }

  // Navigate down in the book list
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (selectedBookIndex < static_cast<int>(bookCount) - 1) {
      selectedBookIndex++;
      changed = true;
    }
  }

  // Open detailed stats (More...)
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Pass the selected book index to the new Activity
    activityManager.pushActivity(
        std::make_unique<DetailedStatsActivity>(renderer, mappedInput, static_cast<uint8_t>(selectedBookIndex)));
    return;
  }

  // Open the selected book directly in the reader
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const BookStatEntry& book = StatsManager.getBook(static_cast<uint8_t>(selectedBookIndex));
    if (book.bookPath[0] != '\0') {
      activityManager.goToReader(std::string(book.bookPath));
    }
    return;
  }

  if (changed) {
    requestUpdate();
  }
}

// -----------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------

void StatsActivity::render(RenderLock&& lock) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  // Draw standard header
  GUI.drawHeader(renderer, Rect(0, metrics.topPadding, screenW, metrics.headerHeight), tr(STR_STATS_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight;
  const int contentH = screenH - contentTop - metrics.buttonHintsHeight;

  // Layout split: Top 25% for global stats, bottom 75% for book list
  const int topH = contentH / 4;
  const int bottomH = contentH - topH;

  renderTopPanel(contentTop, topH, screenW);
  renderBookPanel(contentTop + topH, bottomH, screenW);

  // Draw navigation button hints (Back, Open, More..., [empty])
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_OPEN), tr(STR_STATS_MORE), "");

  renderer.displayBuffer();
}

// -----------------------------------------------------------------------
// Top Panel — Global Statistics (All Time & Last 7 Sessions)
// -----------------------------------------------------------------------
void StatsActivity::renderTopPanel(int panelY, int panelH, int screenW) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& global = StatsManager.getGlobal();
  const int pad = metrics.contentSidePadding;

  // Bottom divider and vertical column separator
  renderer.drawLine(0, panelY + panelH, screenW, panelY + panelH, 1, true);
  renderer.drawLine(screenW / 2, panelY + 8, screenW / 2, panelY + panelH - 8, 1, true);

  const int col1X = pad;
  const int col2X = screenW / 2 + pad;

  // Evenly space 3 rows: headers, time values, session counts
  const int rowStep = panelH / 4;
  const int row1Y = panelY + rowStep / 2;  // Column headers
  const int row2Y = row1Y + rowStep;       // Time values
  const int row3Y = row2Y + rowStep;       // Session values

  // Row 1 — Column headers (Bold)
  renderer.drawText(UI_12_FONT_ID, col1X, row1Y, tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, col2X, row1Y, tr(STR_STATS_LAST_7), true, EpdFontFamily::BOLD);

  // Early exit if no reading data exists yet
  if (global.totalSessionCount == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, panelY + panelH / 2, tr(STR_STATS_NO_DATA), true);
    return;
  }

  // Row 2 — Total time spent
  char bufAllTime[16];
  formatDuration(bufAllTime, sizeof(bufAllTime), global.totalReadingMs);
  renderer.drawText(UI_12_FONT_ID, col1X, row2Y, bufAllTime, true);

  char bufLast7[16];
  formatDuration(bufLast7, sizeof(bufLast7), StatsManager.getLast7SessionsMs());
  renderer.drawText(UI_12_FONT_ID, col2X, row2Y, bufLast7, true);

  // Row 3 — Session counts
  char bufAllSess[24];
  snprintf(bufAllSess, sizeof(bufAllSess), "%u %s", static_cast<unsigned>(global.totalSessionCount),
           tr(STR_STATS_SESSIONS));
  renderer.drawText(UI_12_FONT_ID, col1X, row3Y, bufAllSess, true);

  // char bufLast7Sess[24];                                                                      // commented because of
  // wrong logic snprintf(bufLast7Sess, sizeof(bufLast7Sess), "%u %s",
  //          static_cast<unsigned>(StatsManager.getLast7SessionCount()),
  //          tr(STR_STATS_SESSIONS));
  // renderer.drawText(UI_12_FONT_ID, col2X, row3Y, bufLast7Sess, true);
}

// -----------------------------------------------------------------------
// Bottom Panel — Scrollable Book List
// -----------------------------------------------------------------------
void StatsActivity::renderBookPanel(int panelY, int panelH, int screenW) const {
  const uint8_t count = getVisibleBookCount();

  if (count == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, panelY + panelH / 2, tr(STR_STATS_NO_DATA), true);
    return;
  }

  // Fixed layout showing 3 books per page for optimal cover size and readability
  static constexpr int VISIBLE_ROWS = 3;
  const int rowH = panelH / VISIBLE_ROWS;

  // Calculate scroll offset for page-based navigation (keeps selected book in view)
  const int scrollOffset = (selectedBookIndex / VISIBLE_ROWS) * VISIBLE_ROWS;
  const int visibleCount = std::min(static_cast<int>(count) - scrollOffset, VISIBLE_ROWS);

  // Render each visible row
  for (int i = 0; i < visibleCount; ++i) {
    const int bookIdx = scrollOffset + i;
    const int rowY = panelY + i * rowH;
    renderBookRow(0, rowY, screenW, rowH, StatsManager.getBook(static_cast<uint8_t>(bookIdx)),
                  bookIdx == selectedBookIndex);
  }
}

// -----------------------------------------------------------------------
// Single Book Row Rendering
// -----------------------------------------------------------------------
void StatsActivity::renderBookRow(int rowX, int rowY, int rowW, int rowH, const BookStatEntry& book,
                                  bool selected) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;

  // Top divider for the row
  renderer.drawLine(rowX, rowY, rowX + rowW, rowY, 1, true);

  // Visual highlight for the currently selected book (dithered background)
  if (selected) {
    renderer.fillRectDither(rowX, rowY + 1, rowW, rowH - 1, LightGray);
    renderer.drawRect(rowX, rowY + 1, rowW, rowH - 1, 2, true);
  }

  // Cover dimensions: Portrait rectangle (3:4 aspect ratio to match physical books)
  const int coverH = rowH - COVER_PAD * 2;
  const int coverW = (coverH * 3) / 4;
  const int coverX = rowX + COVER_PAD;
  const int coverY = rowY + COVER_PAD;

  // Attempt to draw the generated cover; fallback to placeholder image if missing
  if (!loadAndDrawCover(coverX, coverY, coverW, coverH, book)) {
    drawCoverPlaceholder(coverX, coverY, coverW, coverH);
  }

  // Text area positioning (starts immediately after the cover)
  const int textX = coverX + coverW + pad + 10;
  const int textW = rowX + rowW - textX - pad - 10;

  // Distribute 4 text lines evenly across the usable row height
  const int line1Y = coverY + 10;  // Title
  const int line2Y = coverY + 40;  // Progress bar
  const int line3Y = coverY + 70;  // Time spent
  const int line4Y = coverY + 95;  // Sessions

  // Line 1 — Book Title (Bold, UI_12)
  renderer.drawText(UI_12_FONT_ID, textX, line1Y, book.title, true, EpdFontFamily::BOLD);

  // Line 2 — Custom Progress Bar + Percentage Text (UI_10)
  // Avoids BaseTheme::drawProgressBar which renders text below the bar
  const int barH = 8;
  const int barY = line2Y + 4;
  const int pctLabelW = 40;                // Reserved width for "100%"
  const int barW = textW - pctLabelW - 5;  // Prevents overlap with text
  const int pctX = textX + barW + 10;
  const int maxX = renderer.getScreenWidth() - 2;

  // Only draw the progress bar if there's enough horizontal space
  if (barW > 10 && pctX < maxX) {
    // Outline
    renderer.drawRect(textX, barY, barW, barH, 1, true);

    // Fill based on percentage
    if (book.progressPercent > 0) {
      const int fillW = (barW * book.progressPercent) / 100;
      if (fillW > 0) {
        renderer.fillRect(textX, barY, fillW, barH, true);
      }
    }

    // Percentage text
    char bufPct[6];
    formatPercent(bufPct, sizeof(bufPct), book.progressPercent);
    renderer.drawText(UI_10_FONT_ID, pctX, line2Y, bufPct, true);
  }

  // Line 3 — Time Spent (UI_10)
  char bufDur[16];
  char bufLine3[40];
  formatDuration(bufDur, sizeof(bufDur), book.totalReadingMs);
  snprintf(bufLine3, sizeof(bufLine3), "%s: %s", tr(STR_STATS_TIME_SPENT), bufDur);
  renderer.drawText(UI_10_FONT_ID, textX, line3Y, bufLine3, true);

  // Line 4 — Session Count (UI_10)
  char bufLine4[24];
  snprintf(bufLine4, sizeof(bufLine4), "%s: %u", tr(STR_STATS_SESSIONS_COUNT),
           static_cast<unsigned>(book.sessionCount));
  renderer.drawText(UI_10_FONT_ID, textX, line4Y, bufLine4, true);
}

// -----------------------------------------------------------------------
// Cover Drawing and Placeholders
// -----------------------------------------------------------------------

/**
 * @brief Draws a fallback placeholder when a book cover is missing.
 * * Attempts to load a default placeholder image (BasicCover.bmp) from the
 * system directory. If the file is missing or corrupt, it falls back to
 * drawing a simple rounded rectangle outline to maintain UI structure.
 */
void StatsActivity::drawCoverPlaceholder(int x, int y, int w, int h) const {
  static constexpr const char* PLACEHOLDER_PATH = "/.crosspoint/system/BasicCover.bmp";

  if (Storage.exists(PLACEHOLDER_PATH)) {
    FsFile f;
    if (Storage.openFileForRead("STATS", PLACEHOLDER_PATH, f)) {
      Bitmap bmp(f, false);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        // Draw a border and fit the image inside
        renderer.drawRect(x, y, w, h, 2, true);
        renderer.drawBitmap(bmp, x + 2, y + 2, w - 4, h - 4);
        f.close();
        return;  // Successfully rendered placeholder
      }
      f.close();
    }
  }

  // Safe fallback if the SD card file is missing or unreadable
  renderer.drawRoundedRect(x, y, w, h, 1, 4, true);
}

/**
 * @brief Loads and renders the thumbnail generated by EpubReaderActivity.
 * * Includes a cascading fallback mechanism for cover resolutions:
 * 1. Tries to load the pixel-perfect 156px height cover (Stats specific).
 * 2. Falls back to the 226px height cover (Home screen specific).
 * 3. Falls back to the legacy 400px height cover.
 * * @return true if a valid cover was found and successfully rendered.
 */
bool StatsActivity::loadAndDrawCover(int x, int y, int w, int h, const BookStatEntry& book) const {
  if (book.thumbBmpPath[0] == '\0') {
    LOG_DBG("STATS", "Cover: thumbBmpPath is empty");
    return false;
  }

  std::string thumbPath;
  bool found = false;

  // Attempt 1: Pixel-perfect 156px version (generated specifically for Stats screen to avoid E-ink scaling artifacts)
  thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), 156);
  if (Storage.exists(thumbPath.c_str())) {
    found = true;
  } else {
    // Attempt 2: Fallback to 226px version (generated for Home screen or manually added)
    thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), 226);
    if (Storage.exists(thumbPath.c_str())) {
      found = true;
    } else {
      // Attempt 3: Legacy fallback to 400px version
      thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), 400);
      if (Storage.exists(thumbPath.c_str())) {
        found = true;
      }
    }
  }

  if (!found) {
    LOG_DBG("STATS", "Cover: No suitable resolution found for '%s'", book.thumbBmpPath);
    return false;
  }

  LOG_DBG("STATS", "Cover: Loading resolved path '%s'", thumbPath.c_str());

  FsFile f;
  if (!Storage.openFileForRead("STATS", thumbPath.c_str(), f)) return false;

  Bitmap bmp(f, false);
  if (bmp.parseHeaders() != BmpReaderError::Ok) {
    f.close();
    return false;
  }

  // Render the loaded bitmap into the specified dimensionss
  renderer.drawBitmap(bmp, x, y, w, h);
  f.close();
  return true;
}