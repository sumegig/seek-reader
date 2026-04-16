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
// Updated to filter based on the current view mode
uint8_t StatsActivity::getVisibleBookCount() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < StatsManager.getBookCount(); ++i) {
    const bool isDone = (StatsManager.getBook(i).progressPercent >= 95);
    // Csak akkor számoljuk, ha megegyezik az aktuális nézettel
    if (isDone == showingFinished) {
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

  // NEW: Toggle between Reading and Finished lists
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    showingFinished = !showingFinished;
    selectedBookIndex = 0;  // Reset scroll position on toggle
    requestUpdate();
    return;
  }

  // --- UPDATED MEMORY MAPPING LOGIC ---
  // Must account for the 'showingFinished' flag when finding the book index
  uint8_t actualMemoryIndex = 0;
  int currentMatch = 0;

  for (uint8_t j = 0; j < StatsManager.getBookCount(); ++j) {
    const bool isDone = (StatsManager.getBook(j).progressPercent >= 95);
    if (isDone != showingFinished) continue;

    if (currentMatch == selectedBookIndex) {
      actualMemoryIndex = j;
      break;
    }
    currentMatch++;
  }

  // Open detailed stats (More...)
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Pass the correctly mapped book index to the new Activity
    activityManager.pushActivity(std::make_unique<DetailedStatsActivity>(renderer, mappedInput, actualMemoryIndex));
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

  // Update button hints based on mode
  // Select the appropriate translation key based on the toggle state
  StrId toggleId = showingFinished ? StrId::STR_STATS_VIEW_READING : StrId::STR_STATS_VIEW_FINISHED;
  const char* toggleHint = I18n::getInstance().get(toggleId);

  // Draw navigation button hints
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_OPEN), tr(STR_STATS_MORE), toggleHint);

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
  // const int row3Y = row2Y + rowStep;       // Session values

  // Column 1: Header - All Time Stats
  renderer.drawText(UI_12_FONT_ID, col1X, row1Y, tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);

  // Column 2: Header - Global Milestone
  renderer.drawText(UI_12_FONT_ID, col2X, row1Y, tr(STR_FINISHED_BOOKS), true, EpdFontFamily::BOLD);

  // Value: Total cumulative reading hours
  char bufAllTime[16];
  formatDuration(bufAllTime, sizeof(bufAllTime), global.totalReadingMs);
  renderer.drawText(UI_12_FONT_ID, col1X, row2Y, bufAllTime, true);

  // Value: Finished Books count
  char bufFinished[12];
  snprintf(bufFinished, sizeof(bufFinished), "%u", static_cast<unsigned>(global.totalBooksFinished));
  renderer.drawText(UI_12_FONT_ID, col2X, row2Y, bufFinished, true);
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

  static constexpr int VISIBLE_ROWS = 3;
  const int rowH = panelH / VISIBLE_ROWS;
  const int scrollOffset = (selectedBookIndex / VISIBLE_ROWS) * VISIBLE_ROWS;
  const int visibleCount = std::min(static_cast<int>(count) - scrollOffset, VISIBLE_ROWS);

  for (int i = 0; i < visibleCount; ++i) {
    const int visibleIdx = scrollOffset + i;
    const int rowY = panelY + i * rowH;

    const BookStatEntry* targetBook = nullptr;
    int currentMatch = 0;

    for (uint8_t j = 0; j < StatsManager.getBookCount(); ++j) {
      const bool isDone = (StatsManager.getBook(j).progressPercent >= 95);

      if (isDone != showingFinished) continue;

      if (currentMatch == visibleIdx) {
        targetBook = &StatsManager.getBook(j);
        break;
      }
      currentMatch++;
    }

    if (targetBook) {
      renderBookRow(0, rowY, screenW, rowH, *targetBook, visibleIdx == selectedBookIndex);
    }
  }
}

// -----------------------------------------------------------------------
// Single Book Row Rendering
// -----------------------------------------------------------------------
// src/activities/stats/StatsActivity.cpp

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

  const int textX = coverX + coverW + pad + 10;
  const int textW = rowX + rowW - textX - pad - 10;
  const int line1Y = coverY + 10;

  // --- FIX: Proper Title Truncation Logic ---
  char truncatedTitle[64];
  size_t titleLen = strlen(book.title);
  // Using a strict 16 character limit to prevent overlap with the percentage text
  static constexpr size_t MAX_LIST_TITLE_LEN = 26;

  if (titleLen > MAX_LIST_TITLE_LEN) {
    strncpy(truncatedTitle, book.title, MAX_LIST_TITLE_LEN);
    truncatedTitle[MAX_LIST_TITLE_LEN] = '\0';
    strcat(truncatedTitle, "...");
  } else {
    strncpy(truncatedTitle, book.title, sizeof(truncatedTitle) - 1);
  }

  // Line 1 — Book Title (Bold, UI_12) - Using the truncated buffer!
  renderer.drawText(UI_12_FONT_ID, textX, line1Y, truncatedTitle, true, EpdFontFamily::BOLD);

  // Distribute text lines evenly across the usable row height
  const int line2Y = coverY + 40;  // Progress bar
  const int line3Y = coverY + 70;  // Time spent
  const int line4Y = coverY + 95;  // Sessions

  // Line 2 — Custom Progress Bar + Percentage Text (UI_10)
  const int barH = 8;
  const int barY = line2Y + 4;
  const int pctLabelW = 40;                // Reserved width for "100%"
  const int barW = textW - pctLabelW - 5;  // Prevents overlap with text
  const int pctX = textX + barW + 10;

  if (barW > 10) {
    renderer.drawRect(textX, barY, barW, barH, 1, true);
    if (book.progressPercent > 0) {
      const int fillW = (barW * book.progressPercent) / 100;
      renderer.fillRect(textX, barY, fillW, barH, true);
    }
    char bufPct[6];
    snprintf(bufPct, sizeof(bufPct), "%u%%", static_cast<unsigned>(book.progressPercent));
    renderer.drawText(UI_10_FONT_ID, pctX, line2Y, bufPct, true);
  }

  // Line 3 — Time Spent (UI_10)
  char bufDur[16];
  char bufLine3[40];
  formatDuration(bufDur, sizeof(bufDur), book.totalReadingMs);
  snprintf(bufLine3, sizeof(bufLine3), "%s: %s", tr(STR_STATS_TIME_SPENT), bufDur);
  renderer.drawText(UI_10_FONT_ID, textX, line3Y, bufLine3, true);

  // Line 4 — Session Count (UI_10)
  char bufLine4[32];
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