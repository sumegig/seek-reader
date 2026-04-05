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
#include "fontIds.h"

// Helper to check current theme
bool HomeActivity::isRecent6Theme() const {
  return SETTINGS.uiTheme == CrossPointSettings::UI_THEME::RECENT6;
}

// ============================================================================
// Original 1D Menu Helpers
// ============================================================================
int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsUrl) {
    count++;
  }
  return count;
}

// ============================================================================
// Data Loading & Helpers (Hybrid)
// ============================================================================

void HomeActivity::loadRecentBooks(int maxBooks) {
  const auto& books = RECENT_BOOKS.getBooks();

  if (isRecent6Theme()) {
    // RECENT6 Theme Loading
    recentBooksRow1.clear();
    recentBooksRow2.clear();
    totalRecentBooks = 0;
    recentBooksRow1.reserve(3);
    recentBooksRow2.reserve(3);

    for (const RecentBook& book : books) {
      if (totalRecentBooks >= maxBooks) break;
      if (!Storage.exists(book.path.c_str())) continue;

      if (recentBooksRow1.size() < 3) {
        recentBooksRow1.push_back(book);
      } else {
        recentBooksRow2.push_back(book);
      }
      totalRecentBooks++;
    }
  } else {
    // Original Theme Loading
    recentBooks.clear();
    recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

    for (const RecentBook& book : books) {
      if (recentBooks.size() >= maxBooks) break;
      if (!Storage.exists(book.path.c_str())) continue;
      recentBooks.push_back(book);
    }
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  if (recentsLoading) return;
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  int progress = 0;

  // Helper lambda to process a single book
  auto processBook = [&](RecentBook& book, int totalBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          epub.load(false, true);

          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / (totalBooks > 0 ? totalBooks : 1)));
          bool success = epub.generateThumbBmp(coverHeight);
          
          if (!success && !isRecent6Theme()) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          
          // Reset rendering flags
          if (isRecent6Theme()) {
             row1Rendered = false;
             row2Rendered = false;
          } else {
             coverRendered = false;
          }
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path) && !isRecent6Theme()) {
          // XTC handling (Original themes only)
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / (totalBooks > 0 ? totalBooks : 1)));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  };

  if (isRecent6Theme()) {
    for (RecentBook& book : recentBooksRow1) processBook(book, totalRecentBooks);
    for (RecentBook& book : recentBooksRow2) processBook(book, totalRecentBooks);
  } else {
    for (RecentBook& book : recentBooks) processBook(book, recentBooks.size());
  }

  recentsLoaded = true;
  recentsLoading = false;
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;

  freeCoverBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));

  if (!coverBuffer) return false;
  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;

  memcpy(frameBuffer, coverBuffer, renderer.getBufferSize());
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
// Navigation Routing (Recent6 specific)
// ============================================================================

void HomeActivity::focusCarousel() {
  carouselFocused = true;
  requestUpdate();
}

void HomeActivity::focusMenu() {
  carouselFocused = false;
  menuSelectedTileIndex = 0;
  requestUpdate();
}

void HomeActivity::launchSelectedActivity() {
  switch (menuSelectedTileIndex) {
    case 0: onFileBrowserOpen(); break;
    case 1: onAppsOpen(); break;
    case 2: onSettingsOpen(); break;
  }
}

// ============================================================================
// Activity Callbacks
// ============================================================================

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }
void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }
void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }
void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }
void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }
void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
void HomeActivity::onAppsOpen() { activityManager.pushActivity(std::make_unique<AppsActivity>(renderer, mappedInput)); }

// ============================================================================
// Lifecycle & Input Loop
// ============================================================================

void HomeActivity::onEnter() {
  Activity::onEnter();

  const auto& metrics = UITheme::getInstance().getMetrics();
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0; 
  
  if (isRecent6Theme()) {
      carouselSelectedIndex = 0;
      carouselFocused = true;
      menuSelectedTileIndex = 0;
      loadRecentBooks(6); // Max 6 for Recent6
  } else {
      selectorIndex = 0;
      loadRecentBooks(metrics.homeRecentBooksCount);
  }
  
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
}

void HomeActivity::loop() {
  if (isRecent6Theme()) {
    // --- RECENT6 LOGIC ---
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

    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (carouselFocused) {
        if (carouselSelectedIndex < 3 && totalRecentBooks > 3) {
          carouselSelectedIndex = std::min(totalRecentBooks - 1, carouselSelectedIndex + 3);
        } else {
          focusMenu();
        }
      } else if (menuSelectedTileIndex == 0 || menuSelectedTileIndex == 1) {
        menuSelectedTileIndex = 2;
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (!carouselFocused) {
        if (menuSelectedTileIndex == 2) {
          menuSelectedTileIndex = 0;
        } else {
          carouselFocused = true;
          carouselSelectedIndex = (totalRecentBooks > 3) ? 3 + std::min(2, menuSelectedTileIndex)
                                                         : std::min(totalRecentBooks - 1, menuSelectedTileIndex);
          if (carouselSelectedIndex >= totalRecentBooks) carouselSelectedIndex = totalRecentBooks - 1;
        }
      } else if (carouselSelectedIndex >= 3) {
        carouselSelectedIndex -= 3;
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      int totalElements = totalRecentBooks + 3;
      int globalIndex = carouselFocused ? carouselSelectedIndex : (totalRecentBooks + menuSelectedTileIndex);

      if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        globalIndex = (globalIndex + 1) % totalElements;
      } else {
        globalIndex = (globalIndex - 1 + totalElements) % totalElements;
      }

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
    
  } else {
    // --- ORIGINAL LOGIC ---
    const int menuCount = getMenuItemCount();

    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      int idx = 0;
      int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
      const int fileBrowserIdx = idx++;
      const int recentsIdx = idx++;
      const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
      const int fileTransferIdx = idx++;
      const int settingsIdx = idx;

      if (selectorIndex < recentBooks.size()) {
        onSelectBook(recentBooks[selectorIndex].path);
      } else if (menuSelectedIndex == fileBrowserIdx) {
        onFileBrowserOpen();
      } else if (menuSelectedIndex == recentsIdx) {
        onRecentsOpen();
      } else if (menuSelectedIndex == opdsLibraryIdx) {
        onOpdsBrowserOpen();
      } else if (menuSelectedIndex == fileTransferIdx) {
        onFileTransferOpen();
      } else if (menuSelectedIndex == settingsIdx) {
        onSettingsOpen();
      }
    }
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  if (isRecent6Theme()) {
    // --- RECENT 6 RENDERING ---
    const int startY = metrics.homeTopPadding;
    const int bookRowHeight = metrics.homeCoverTileHeight - 25;

    int activeSel1 = (carouselFocused && carouselSelectedIndex < 3) ? carouselSelectedIndex : -1;
    int activeSel2 = (carouselFocused && carouselSelectedIndex >= 3) ? (carouselSelectedIndex - 3) : -1;

    int renderSel1 = coverBufferStored ? activeSel1 : -1;
    int renderSel2 = coverBufferStored ? activeSel2 : -1;

    GUI.drawRecentBookCover(renderer, Rect(0, startY, pageWidth, bookRowHeight), recentBooksRow1, renderSel1,
                            row1Rendered, row1Stored, bufferRestored, [&]() { return true; });

    if (!recentBooksRow2.empty()) {
      GUI.drawRecentBookCover(renderer, Rect(0, startY + bookRowHeight + 10, pageWidth, bookRowHeight), recentBooksRow2,
                              renderSel2, row2Rendered, row2Stored, bufferRestored, [&]() { return true; });
    } else {
      row2Rendered = true;
    }

    if (row1Rendered && row2Rendered && !coverBufferStored) {
      storeCoverBuffer();
      coverBufferStored = true;
      bool tempRestored = true;

      GUI.drawRecentBookCover(renderer, Rect(0, startY, pageWidth, bookRowHeight), recentBooksRow1, activeSel1,
                              row1Rendered, row1Stored, tempRestored, [&]() { return true; });

      if (!recentBooksRow2.empty()) {
        GUI.drawRecentBookCover(renderer, Rect(0, startY + bookRowHeight + 10, pageWidth, bookRowHeight), recentBooksRow2,
                                activeSel2, row2Rendered, row2Stored, tempRestored, [&]() { return true; });
      }
    }

    // Asymmetrical Menu
    const int menuRowHeight = 40;
    const int menuGap = 10;
    const int bottomMargin = 10;
    const int padding = metrics.contentSidePadding;
    const int menuY = pageHeight - (menuRowHeight * 2) - menuGap - metrics.buttonHintsHeight - bottomMargin;

    auto drawCustomMenuBtn = [&](int index, const char* label, int x, int y, int w, int h) {
      bool isSelected = (!carouselFocused && menuSelectedTileIndex == index);
      if (isSelected) {
        renderer.fillRect(x, y, w, h);
      } else {
        renderer.drawRect(x, y, w, h);
      }
      int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
      int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
      renderer.drawText(UI_10_FONT_ID, x + (w - textWidth) / 2, y + (h - textHeight) / 2, label, !isSelected);
    };

    int width0 = (pageWidth / 2) - padding - (menuGap / 2);
    drawCustomMenuBtn(0, tr(STR_BROWSE_FILES), padding, menuY, width0, menuRowHeight);

    int startX1 = (pageWidth / 2) + (menuGap / 2);
    int width1 = (pageWidth / 2) - padding - (menuGap / 2);
    drawCustomMenuBtn(1, tr(STR_APPS), startX1, menuY, width1, menuRowHeight);

    int width2 = pageWidth - (padding * 2);
    drawCustomMenuBtn(2, tr(STR_SETTINGS_TITLE), padding, menuY + menuRowHeight + menuGap, width2, menuRowHeight);

    const auto labels = carouselFocused ? mappedInput.mapLabels("", tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                                        : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else {
    // --- ORIGINAL RENDERING ---
    GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                            recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                            std::bind(&HomeActivity::storeCoverBuffer, this));

    std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                          tr(STR_SETTINGS_TITLE)};
    std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

    if (hasOpdsUrl) {
      menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
      menuIcons.insert(menuIcons.begin() + 2, Library);
    }

    GUI.drawButtonMenu(
        renderer,
        Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
             pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                           metrics.buttonHintsHeight)},
        static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
        [&menuItems](int index) { return std::string(menuItems[index]); },
        [&menuIcons](int index) { return menuIcons[index]; });

    const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}