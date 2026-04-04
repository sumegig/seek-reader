#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/AppsActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

// ============================================================================
// Data Loading & Helpers
// ============================================================================

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooksRow1.clear();
  recentBooksRow2.clear();
  totalRecentBooks = 0;

  const auto& books = RECENT_BOOKS.getBooks();

  // Prevent DRAM fragmentation by pre-allocating known bounds
  recentBooksRow1.reserve(3);
  recentBooksRow2.reserve(3);

  for (const RecentBook& book : books) {
    if (totalRecentBooks >= maxBooks) break;

    // Ensure file still exists on the SD card
    if (!Storage.exists(book.path.c_str())) continue;

    // Distribute books across the two-row grid
    if (recentBooksRow1.size() < 3) {
      recentBooksRow1.push_back(book);
    } else {
      recentBooksRow2.push_back(book);
    }
    totalRecentBooks++;
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  if (recentsLoading) return;
  recentsLoading = true;

  bool showingLoading = false;
  Rect popupRect(0, 0, 0, 0);
  int progress = 0;

  // Helper lambda to process cover generation sequentially
  auto processRow = [&](std::vector<RecentBook>& row) {
    for (RecentBook& book : row) {
      if (!book.coverBmpPath.empty()) {
        std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (!Storage.exists(coverPath.c_str())) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            epub.load(false, true);

            // Display loading popup during potentially long EPUB parsing
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect,
                                  10 + progress * (90 / (totalRecentBooks > 0 ? totalRecentBooks : 1)));
            epub.generateThumbBmp(coverHeight);

            // Reset rendering flags to force a UI update for the newly generated cover
            row1Rendered = false;
            row2Rendered = false;
            coverBufferStored = false;
            requestUpdate();
          }
        }
      }
      progress++;
    }
  };

  processRow(recentBooksRow1);
  processRow(recentBooksRow2);

  recentsLoaded = true;
  recentsLoading = false;
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;

  // Free existing buffer to prevent memory leaks
  freeCoverBuffer();
  const size_t bufferSize = GfxRenderer::getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));

  if (!coverBuffer) return false;

  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;

  memcpy(frameBuffer, coverBuffer, GfxRenderer::getBufferSize());
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

// ============================================================================
// Navigation Routing
// ============================================================================

void HomeActivity::focusCarousel() {
  carouselFocused = true;
  requestUpdate();
}

void HomeActivity::focusMenu() {
  carouselFocused = false;
  menuSelectedTileIndex = 0;  // Default to Browse Files
  requestUpdate();
}

void HomeActivity::launchSelectedActivity() {
  switch (menuSelectedTileIndex) {
    case 0:
      onFileBrowserOpen();
      break;
    case 1:
      onAppsOpen();
      break;
    case 2:
      onSettingsOpen();
      break;
  }
}

// ============================================================================
// Activity Callbacks
// ============================================================================

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }
void HomeActivity::onFileBrowserOpen() {
  activityManager.pushActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput));
}
void HomeActivity::onAppsOpen() { activityManager.pushActivity(std::make_unique<AppsActivity>(renderer, mappedInput)); }
void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

// ============================================================================
// Lifecycle & Input Loop
// ============================================================================

void HomeActivity::onEnter() {
  Activity::onEnter();
  carouselSelectedIndex = 0;
  carouselFocused = true;
  menuSelectedTileIndex = 0;

  // Load up to 6 books for the 3x2 grid layout
  loadRecentBooks(6);
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
}

void HomeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (carouselFocused && totalRecentBooks > 0) {
      std::string path = (carouselSelectedIndex < 3) ? recentBooksRow1[carouselSelectedIndex].path
                                                     : recentBooksRow2[carouselSelectedIndex - 3].path;
      onSelectBook(path);
    } else if (!carouselFocused) {
      launchSelectedActivity();
    }
    return;
  }

  // ---------------------------------------------------------
  // Vertical Navigation: Preserved specific grid logic
  // ---------------------------------------------------------
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (carouselFocused) {
      if (carouselSelectedIndex < 3 && totalRecentBooks > 3) {
        // Move from first row of books to second row
        carouselSelectedIndex = std::min(totalRecentBooks - 1, carouselSelectedIndex + 3);
      } else {
        focusMenu();
      }
    } else if (menuSelectedTileIndex == 0 || menuSelectedTileIndex == 1) {
      // Move from Browse/Apps down to Settings
      menuSelectedTileIndex = 2;
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!carouselFocused) {
      if (menuSelectedTileIndex == 2) {
        // Move from Settings up to Browse Files
        menuSelectedTileIndex = 0;
      } else {
        // Move from top menu row to books
        carouselFocused = true;
        carouselSelectedIndex = (totalRecentBooks > 3) ? 3 + std::min(2, menuSelectedTileIndex)
                                                       : std::min(totalRecentBooks - 1, menuSelectedTileIndex);
        if (carouselSelectedIndex >= totalRecentBooks) carouselSelectedIndex = totalRecentBooks - 1;
      }
    } else if (carouselSelectedIndex >= 3) {
      // Move from second row of books to first row
      carouselSelectedIndex -= 3;
    }
    requestUpdate();
    return;
  }

  // ---------------------------------------------------------
  // Horizontal Navigation: Continuous cycle (wrapping)
  // ---------------------------------------------------------
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Calculate global index: 0 to (totalRecentBooks - 1) = Books
    // totalRecentBooks to (totalRecentBooks + 2) = Menu Tiles
    int totalElements = totalRecentBooks + 3;  // 3 menu options
    int globalIndex = carouselFocused ? carouselSelectedIndex : (totalRecentBooks + menuSelectedTileIndex);

    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      globalIndex = (globalIndex + 1) % totalElements;
    } else {
      globalIndex = (globalIndex - 1 + totalElements) % totalElements;
    }

    // Re-apply global index to specific grid states
    if (globalIndex < totalRecentBooks) {
      carouselFocused = true;
      carouselSelectedIndex = globalIndex;
    } else {
      carouselFocused = false;
      menuSelectedTileIndex = globalIndex - totalRecentBooks;
    }

    requestUpdate();
    return;
  }
}

// ============================================================================
// Rendering
// ============================================================================

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect(0, metrics.topPadding, pageWidth, metrics.homeTopPadding), nullptr);

  // ============================================================
  // 1. Book Grid (Slightly reduced height to accommodate long titles)
  // ============================================================
  const int startY = metrics.homeTopPadding;

  // 25 pixels smaller box: the UI engine will proportionally scale down the cover,
  // leaving plenty of room for the text to avoid overlapping.
  const int bookRowHeight = metrics.homeCoverTileHeight - 25;

  // Determine actual active selections
  int activeSel1 = (carouselFocused && carouselSelectedIndex < 3) ? carouselSelectedIndex : -1;
  int activeSel2 = (carouselFocused && carouselSelectedIndex >= 3) ? (carouselSelectedIndex - 3) : -1;

  // Prevent baking the selection background into the cached buffer!
  // If the buffer hasn't been stored yet, we force a clean render (-1) without any focus highlights.
  int renderSel1 = coverBufferStored ? activeSel1 : -1;
  int renderSel2 = coverBufferStored ? activeSel2 : -1;

  // Render ROW 1
  GUI.drawRecentBookCover(renderer, Rect(0, startY, pageWidth, bookRowHeight), recentBooksRow1, renderSel1,
                          row1Rendered, row1Stored, bufferRestored, [&]() { return true; });

  // Render ROW 2 (With a 10px gap below the first row)
  if (!recentBooksRow2.empty()) {
    GUI.drawRecentBookCover(renderer, Rect(0, startY + bookRowHeight + 10, pageWidth, bookRowHeight), recentBooksRow2,
                            renderSel2, row2Rendered, row2Stored, bufferRestored, [&]() { return true; });
  } else {
    row2Rendered = true;
  }

  // Save the clean (unfocused) framebuffer, then instantly draw the focus highlights on top
  if (row1Rendered && row2Rendered && !coverBufferStored) {
    storeCoverBuffer();
    coverBufferStored = true;

    bool tempRestored = true;

    // Draw actual highlights for Row 1
    GUI.drawRecentBookCover(renderer, Rect(0, startY, pageWidth, bookRowHeight), recentBooksRow1, activeSel1,
                            row1Rendered, row1Stored, tempRestored, [&]() { return true; });

    // Draw actual highlights for Row 2
    if (!recentBooksRow2.empty()) {
      GUI.drawRecentBookCover(renderer, Rect(0, startY + bookRowHeight + 10, pageWidth, bookRowHeight), recentBooksRow2,
                              activeSel2, row2Rendered, row2Stored, tempRestored, [&]() { return true; });
    }
  }

  // ============================================================
  // 2. Exact Asymmetrical Menu (Manual Box Placement)
  // ============================================================
  const int menuRowHeight = 40;
  const int menuGap = 10;

  // Margin reduced to 10px to anchor the menu closer to the bottom edge
  const int bottomMargin = 10;
  const int padding = metrics.contentSidePadding;

  const int menuY = pageHeight - (menuRowHeight * 2) - menuGap - metrics.buttonHintsHeight - bottomMargin;

  // Lambda helper for drawing a single menu button (memory efficient)
  auto drawCustomMenuBtn = [&](int index, const char* label, int x, int y, int w, int h) {
    bool isSelected = (!carouselFocused && menuSelectedTileIndex == index);

    // Draw filled black box if selected
    if (isSelected) {
      renderer.fillRect(x, y, w, h);
    } else {
      renderer.drawRect(x, y, w, h);
    }

    // Center text within the bounding box
    int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    int textHeight = renderer.getLineHeight(UI_10_FONT_ID);

    // '!isSelected' inverts text color: white text on black background when selected
    renderer.drawText(UI_10_FONT_ID, x + (w - textWidth) / 2, y + (h - textHeight) / 2, label, !isSelected);
  };

  // 2.1: BROWSE FILES (Left half of the screen minus half gap)
  int width0 = (pageWidth / 2) - padding - (menuGap / 2);
  drawCustomMenuBtn(0, tr(STR_BROWSE_FILES), padding, menuY, width0, menuRowHeight);

  // 2.2: APPS (Right half of the screen)
  int startX1 = (pageWidth / 2) + (menuGap / 2);
  int width1 = (pageWidth / 2) - padding - (menuGap / 2);
  drawCustomMenuBtn(1, tr(STR_APPS), startX1, menuY, width1, menuRowHeight);

  // 2.3: SETTINGS (Bottom row, full width)
  int width2 = pageWidth - (padding * 2);
  drawCustomMenuBtn(2, tr(STR_SETTINGS_TITLE), padding, menuY + menuRowHeight + menuGap, width2, menuRowHeight);

  // ============================================================
  // 3. Context-sensitive Button Hints
  // ============================================================
  const auto labels = carouselFocused ? mappedInput.mapLabels("", tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                                      : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // Trigger deferred cover generation with the original dimensions
  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}