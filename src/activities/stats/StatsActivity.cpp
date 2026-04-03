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
    // Filter logic aligns with HomeActivity to handle floating point truncation
    if (StatsManager.getBook(i).progressPercent < 95) {
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

  // --- MEMORY MAPPING LOGIC ---
  // We need to map the visual UI index (selectedBookIndex) to the actual index
  // in the StatsManager memory, explicitly skipping finished books (>= 95%).
  uint8_t actualMemoryIndex = 0;
  int currentUnfinished = 0;
  
  for (uint8_t j = 0; j < StatsManager.getBookCount(); ++j) {
    if (StatsManager.getBook(j).progressPercent >= 95) continue;
    
    if (currentUnfinished == selectedBookIndex) {
      actualMemoryIndex = j;
      break;
    }
    currentUnfinished++;
  }

  // Open detailed stats (More...)
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Pass the correctly mapped book index to the new Activity
    activityManager.pushActivity(
        std::make_unique<DetailedStatsActivity>(renderer, mappedInput, actualMemoryIndex));
    return;
  }

  // Open the selected book directly in the reader
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const BookStatEntry& book = StatsManager.getBook(actualMemoryIndex);
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
    const int visibleIdx = scrollOffset + i;
    const int rowY = panelY + i * rowH;

    // --- MEMORY MAPPING LOGIC ---
    // Safely retrieve the actual book from memory, jumping over finished books
    const BookStatEntry* targetBook = nullptr;
    int currentUnfinished = 0;
    
    for (uint8_t j = 0; j < StatsManager.getBookCount(); ++j) {
      if (StatsManager.getBook(j).progressPercent >= 95) continue;
      
      if (currentUnfinished == visibleIdx) {
        targetBook = &StatsManager.getBook(j);
        break;
      }
      currentUnfinished++;
    }

    // Render the row using the mapped book
    if (targetBook) {
      renderBookRow(0, rowY, screenW, rowH, *targetBook, visibleIdx == selectedBookIndex);
    }
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
 */
void StatsActivity::drawCoverPlaceholder(int x, int y, int w, int h) const {
  // CRITICAL: Ensure the 'fill' parameter is 'false' so we do not draw a black box over the area.
  renderer.drawRoundedRect(x, y, w, h, 1, 4, false);
}

/**
 * @brief Loads and renders the thumbnail generated by EpubReaderActivity.
 * * Includes a cascading fallback mechanism synchronized with the HomeActivity architecture.
 * * @return true if a valid cover was found and successfully rendered.
 */
bool StatsActivity::loadAndDrawCover(int x, int y, int w, int h, const BookStatEntry& book) const {
  if (book.thumbBmpPath[0] == '\0') {
    LOG_DBG("STATS", "Cover: thumbBmpPath is empty");
    return false;
  }

  std::string thumbPath;
  bool found = false;

  // The fallback array matches the target resolutions configured in HomeActivity
  std::vector<int> fallbacks = {156, 234, 226, 340, 400};
  for (int res : fallbacks) {
    thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), res);
    if (Storage.exists(thumbPath.c_str())) {
      found = true;
      break;
    }
  }

  if (!found) {
    LOG_DBG("STATS", "Cover: No suitable resolution found for '%s'", book.thumbBmpPath);
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead("STATS", thumbPath.c_str(), f)) return false;

  Bitmap bmp(f, false);
  if (bmp.parseHeaders() != BmpReaderError::Ok) {
    f.close();
    return false;
  }

  // Clear area with white (false) before rendering the image
  renderer.fillRect(x, y, w, h, false);
  
  // Fast Path: Direct draw. The scaling artifacts on the E-ink screen are minimized here since
  // the Stats layout uses aspect fit by default.
  renderer.drawBitmap(bmp, x, y, w, h);
  
  f.close();
  return true;
}