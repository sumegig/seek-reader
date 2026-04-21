#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_system.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"  // added when developing Statistics menu
#include "util/ScreenshotUtil.h"

// Dictionary development
#include "DictionaryWordSelectActivity.h"
#include "LookedUpWordsActivity.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// pages per minute, first item is 1 to prevent division by zero if accessed
const std::vector<int> PAGE_TURN_LABELS = {1, 1, 3, 6, 12};

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  sessionPagesTurned = 0;  // RESET: Ensure counter starts at zero for every session

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
  // Generate thumbnail for Home screen
  constexpr int HOME_THUMB_HEIGHT = 226;
  if (!Storage.exists(epub->getThumbBmpPath(HOME_THUMB_HEIGHT).c_str())) {
    epub->generateThumbBmp(HOME_THUMB_HEIGHT);
  }

  // Generate thumbnail exactly for Stats cover display to avoid pixelation
  constexpr int STATS_THUMB_HEIGHT = 156;
  if (!Storage.exists(epub->getThumbBmpPath(STATS_THUMB_HEIGHT).c_str())) {
    epub->generateThumbBmp(STATS_THUMB_HEIGHT);
  }

  StatsManager.beginSession(epub->getCachePath().c_str(), epub->getTitle().c_str(),
                            epub->getAuthor().c_str(),  // new
                            epub->getPath().c_str(), epub->getThumbBmpPath().c_str(),
                            static_cast<uint8_t>(epub->calculateProgress(currentSpineIndex, 0.0f) * 100.0f));
}

void EpubReaderActivity::onExit() {
  // End reading stats session — compute current progress
  {
    const float chapterProg = (section && section->pageCount > 0)
                                  ? static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount)
                                  : 0.0f;
    const uint8_t prog = static_cast<uint8_t>(epub->calculateProgress(currentSpineIndex, chapterProg) * 100.0f);
    StatsManager.endSession(prog, sessionPagesTurned);  // new
  }
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // --- QUICK SETTINGS INTERCEPT ---
  // Trap all inputs if the Quick Settings overlay is open.
  if (qsState != QuickSettingsState::CLOSED) {
    handleQuickSettingsInput();
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

    const bool hasDictionary = Dictionary::exists();
    const bool hasLookupHistory = hasDictionary && LookupHistory::hasHistory(epub->getCachePath());

    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty(), hasDictionary, hasLookupHistory),

                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             toggleAutoPageTurn(menu.pageTurnOption);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // any botton press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    requestUpdate();
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    lastPageTurnTime = millis();
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }

    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP: {
      std::unique_ptr<Page> pageForLookup;
      std::string nextPageFirstWord;
      int orientedMarginTop = 0;
      int orientedMarginLeft = 0;

      {
        RenderLock lock(*this);
        if (!section) {
          requestUpdate();
          break;
        }

        int orientedMarginRight;
        int orientedMarginBottom;
        renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                         &orientedMarginLeft);

        orientedMarginTop += SETTINGS.screenMargin;
        orientedMarginLeft += SETTINGS.screenMargin;
        orientedMarginRight += SETTINGS.screenMargin;

        const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
        if (automaticPageTurnActive &&
            (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
          orientedMarginBottom += std::max(
              SETTINGS.screenMargin,
              static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
        } else {
          orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
        }

        pageForLookup = section->loadPageFromSectionFile();

        if (section->currentPage < section->pageCount - 1) {
          const int savedPage = section->currentPage;
          section->currentPage = savedPage + 1;
          auto nextPage = section->loadPageFromSectionFile();
          section->currentPage = savedPage;

          if (nextPage) {
            for (const auto& element : nextPage->elements) {
              if (!element || element->getTag() != TAG_PageLine) continue;

              const auto& line = static_cast<const PageLine&>(*element);
              auto block = line.getBlock();
              if (!block) continue;

              const auto& words = block->getWords();

              if (!words.empty()) {
                nextPageFirstWord = words.front();
                break;
              }
            }
          }
        }
      }

      if (pageForLookup) {
        startActivityForResult(
            std::make_unique<DictionaryWordSelectActivity>(
                renderer, mappedInput, std::move(pageForLookup), SETTINGS.getReaderFontId(), orientedMarginLeft,
                orientedMarginTop, epub->getCachePath(), SETTINGS.orientation, nextPageFirstWord),
            [this](const ActivityResult& result) { requestUpdate(); });
      }
      break;
    }

    case EpubReaderMenuActivity::MenuAction::LOOKED_UP_WORDS: {
      startActivityForResult(std::make_unique<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this](const ActivityResult& result) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::QUICK_SETTINGS: {
      // Capture current global settings to local temporary buffer
      tempSettings.fontFamily = SETTINGS.fontFamily;
      // ... (a többi másolás marad ugyanígy) ...
      tempSettings.shortPwrBtn = SETTINGS.shortPwrBtn;

      qsState = QuickSettingsState::TAB_FOCUSED;
      qsSelectedTab = 0;
      qsSelectedItem = 0;
      qsScrollOffset = 0;

      qsNeedsBackgroundRender = true;

      requestUpdate();
      break;
    }

    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;

        // --- ADDED: Read exact DOM path from the SD Card cache ---
        std::string exactXPath = "";
        if (section && currentPage >= 0 && currentPage < totalPages) {
          auto p = section->loadPageFromSectionFile();
          if (p) exactXPath = p->syncXPath;
        }

        startActivityForResult(
            std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                                   currentPage, totalPages, exactXPath),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& sync = std::get<SyncResult>(result.data);
                if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
                  RenderLock lock(*this);
                  currentSpineIndex = sync.spineIndex;
                  nextPageNumber = sync.page;
                  section.reset();
                }
              }
            });
      }
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_LABELS.size()) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    sessionPagesTurned++;  // new
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  if (qsState != QuickSettingsState::CLOSED && !qsNeedsBackgroundRender) {
    renderQuickSettingsOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  // Margók és viewport számolás
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering)) {
      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, popupFn)) {
        section.reset();
        return;
      }
    }

    if (nextPageNumber == UINT16_MAX)
      section->currentPage = section->pageCount - 1;
    else
      section->currentPage = nextPageNumber;

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) section->currentPage = *page;
      pendingAnchor.clear();
    }

    if (cachedChapterTotalPageCount > 0) {
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        section->currentPage = static_cast<int>(progress * section->pageCount);
      }
      cachedChapterTotalPageCount = 0;
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0 || section->currentPage < 0 || section->currentPage >= section->pageCount) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300,
                              section->pageCount == 0 ? tr(STR_EMPTY_CHAPTER) : tr(STR_OUT_OF_BOUNDS), true,
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      section->clearCache();
      section.reset();
      requestUpdate();
      automaticPageTurnActive = false;
      return;
    }

    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();

    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }

  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  if (qsState != QuickSettingsState::CLOSED) {
    renderQuickSettingsOverlay();

    if (qsNeedsBackgroundRender) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    qsNeedsBackgroundRender = false;
  } else {
    // Only process standard full refresh logic if overlay is closed
    silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  renderer.clearScreen();

  if (section->pageCount == 0 || section->currentPage < 0 || section->currentPage >= section->pageCount) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300,
                              section->pageCount == 0 ? tr(STR_EMPTY_CHAPTER) : tr(STR_OUT_OF_BOUNDS), true,
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      section->clearCache();
      section.reset();
      requestUpdate();
      automaticPageTurnActive = false;
      return;
    }

    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();

    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }

  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  if (qsState != QuickSettingsState::CLOSED) {
    renderQuickSettingsOverlay();

    if (qsNeedsBackgroundRender) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    qsNeedsBackgroundRender = false;
  } else {
    // Only process standard full refresh logic if overlay is closed
    silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);

  // --- QUICK SETTINGS OPTIMIZATION FLAG ---
  // If the overlay is open, we suppress display updates and heavy grayscale logic
  // to ensure instantaneous, flicker-free menu navigation.
  const bool isQsOpen = (qsState != QuickSettingsState::CLOSED);

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  fcm->logStats("bw_render");
  const auto tBwRender = millis();

  // ONLY push to display here if Quick Settings is CLOSED
  if (imagePageWithAA) {
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      if (!isQsOpen) renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      if (!isQsOpen) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      if (!isQsOpen) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  } else {
    if (!isQsOpen) ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();
  const auto tBwStore = millis();

  // grayscale rendering
  // SKIP grayscale completely if QuickSettings is open to ensure instant menu navigation!
  if (SETTINGS.textAntiAliasing && !isQsOpen) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    fcm->logStats("gray");

    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    // restore the bw data (Menu will be drawn on top of this later in render() )
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

// ============================================================================
// QUICK SETTINGS OVERLAY IMPLEMENTATION
// ============================================================================

int EpubReaderActivity::getQsItemCount(int tab) const {
  return (tab == 0) ? 9 : 3;  // 9 Reader settings, 3 Controls settings
}

const char* EpubReaderActivity::getQsItemName(int tab, int index) const {
  if (tab == 0) {
    switch (index) {
      case 0:
        return tr(STR_FONT_FAMILY);
      case 1:
        return tr(STR_FONT_SIZE);
      case 2:
        return tr(STR_LINE_SPACING);
      case 3:
        return tr(STR_SCREEN_MARGIN);
      case 4:
        return tr(STR_PARA_ALIGNMENT);
      case 5:
        return tr(STR_EMBEDDED_STYLE);
      case 6:
        return tr(STR_HYPHENATION);
      case 7:
        return tr(STR_EXTRA_SPACING);
      case 8:
        return tr(STR_TEXT_AA);
    }
  } else {
    switch (index) {
      case 0:
        return tr(STR_SIDE_BTN_LAYOUT);
      case 1:
        return tr(STR_LONG_PRESS_SKIP);
      case 2:
        return tr(STR_SHORT_PWR_BTN);
    }
  }
  return "";
}

const char* EpubReaderActivity::getQsItemValue(int tab, int index, char* tempBuf, size_t tempBufSize) const {
  auto onOff = [](uint8_t val) { return val ? tr(STR_STATE_ON) : tr(STR_STATE_OFF); };
  if (tab == 0) {
    switch (index) {
      case 0:
        return (tempSettings.fontFamily == 0)   ? tr(STR_BOOKERLY)
               : (tempSettings.fontFamily == 1) ? tr(STR_NOTO_SANS)
                                                : tr(STR_OPEN_DYSLEXIC);
      case 1:
        return (tempSettings.fontSize == 0)   ? tr(STR_SMALL)
               : (tempSettings.fontSize == 1) ? tr(STR_MEDIUM)
               : (tempSettings.fontSize == 2) ? tr(STR_LARGE)
                                              : tr(STR_X_LARGE);
      case 2:
        return (tempSettings.lineSpacing == 0)   ? tr(STR_TIGHT)
               : (tempSettings.lineSpacing == 1) ? tr(STR_NORMAL)
                                                 : tr(STR_WIDE);
      case 3:
        snprintf(tempBuf, tempBufSize, "%d %s", tempSettings.screenMargin, tr(STR_PX));
        return tempBuf;

      case 4:
        return (tempSettings.paragraphAlignment == 0)   ? tr(STR_JUSTIFY)
               : (tempSettings.paragraphAlignment == 1) ? tr(STR_ALIGN_LEFT)
               : (tempSettings.paragraphAlignment == 2) ? tr(STR_CENTER)
                                                        : tr(STR_ALIGN_RIGHT);
      case 5:
        return onOff(tempSettings.embeddedStyle);
      case 6:
        return onOff(tempSettings.hyphenationEnabled);
      case 7:
        return onOff(tempSettings.extraParagraphSpacing);
      case 8:
        return onOff(tempSettings.textAntiAliasing);
    }
  } else {
    switch (index) {
      case 0:
        return (tempSettings.sideButtonLayout == 0) ? tr(STR_PREV_NEXT) : tr(STR_NEXT_PREV);
      case 1:
        return tempSettings.longPressChapterSkip ? tr(STR_CHAPTER) : tr(STR_SCROLL);
      case 2:
        return (tempSettings.shortPwrBtn == 0)   ? tr(STR_IGNORE)
               : (tempSettings.shortPwrBtn == 1) ? tr(STR_SLEEP)
                                                 : tr(STR_PAGE_TURN);
    }
  }
  return "";
}

void EpubReaderActivity::adjustQsItemValue(int tab, int index, bool increment) {
  // Helper lambda for wrapping enums securely
  auto cycle = [](uint8_t& val, int maxVal, bool inc) {
    if (inc)
      val = (val + 1) % maxVal;
    else
      val = (val == 0) ? maxVal - 1 : val - 1;
  };

  if (tab == 0) {
    switch (index) {
      case 0:
        cycle(tempSettings.fontFamily, 3, increment);
        break;
      case 1:
        cycle(tempSettings.fontSize, 4, increment);
        break;
      case 2:
        cycle(tempSettings.lineSpacing, 3, increment);
        break;
      case 3: {  // Margin (5 to 40, step 5)
        uint8_t& m = tempSettings.screenMargin;
        if (increment)
          m = (m >= 40) ? 5 : m + 5;
        else
          m = (m <= 5) ? 40 : m - 5;
        break;
      }
      case 4:
        cycle(tempSettings.paragraphAlignment, 4, increment);
        break;
      // uint8_t based toggles (0 or 1)
      case 5:
        tempSettings.embeddedStyle = tempSettings.embeddedStyle ? 0 : 1;
        break;
      case 6:
        tempSettings.hyphenationEnabled = tempSettings.hyphenationEnabled ? 0 : 1;
        break;
      case 7:
        tempSettings.extraParagraphSpacing = tempSettings.extraParagraphSpacing ? 0 : 1;
        break;
      case 8:
        tempSettings.textAntiAliasing = tempSettings.textAntiAliasing ? 0 : 1;
        break;
    }
  } else {
    switch (index) {
      case 0:
        cycle(tempSettings.sideButtonLayout, 2, increment);
        break;
      case 1:
        tempSettings.longPressChapterSkip = tempSettings.longPressChapterSkip ? 0 : 1;
        break;
      case 2:
        cycle(tempSettings.shortPwrBtn, 3, increment);
        break;
    }
  }
}

void EpubReaderActivity::handleQuickSettingsInput() {
  const int itemCount = getQsItemCount(qsSelectedTab);

  if (qsState == QuickSettingsState::TAB_FOCUSED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      qsSelectedTab = (qsSelectedTab == 0) ? 1 : 0;
      qsSelectedItem = 0;
      qsScrollOffset = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      qsState = QuickSettingsState::ITEM_FOCUSED;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      qsState = QuickSettingsState::ITEM_FOCUSED;
      qsSelectedItem = itemCount - 1;
      // MAX_VISIBLE = 5 miatt itt a láthatósági ablak mérete 4 (0-tól indexelve)
      qsScrollOffset = std::max(0, qsSelectedItem - 4);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      qsState = QuickSettingsState::CLOSED;  // Discard changes
      pagesUntilFullRefresh = 0;             // Kényszerített tiszta frissítés kilépéskor
      requestUpdate();
    }
  } else if (qsState == QuickSettingsState::ITEM_FOCUSED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      qsSelectedItem--;
      if (qsSelectedItem < 0) {
        qsState = QuickSettingsState::TAB_FOCUSED;
        qsSelectedItem = 0;
        qsScrollOffset = 0;
      } else if (qsSelectedItem < qsScrollOffset) {
        qsScrollOffset = qsSelectedItem;
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      qsSelectedItem++;
      if (qsSelectedItem >= itemCount) {
        qsSelectedItem = 0;  // Wrap around to top
        qsScrollOffset = 0;
      } else if (qsSelectedItem > qsScrollOffset + 4) {  // MAX_VISIBLE - 1
        qsScrollOffset = qsSelectedItem - 4;
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      adjustQsItemValue(qsSelectedTab, qsSelectedItem, false);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      adjustQsItemValue(qsSelectedTab, qsSelectedItem, true);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      applyQuickSettings();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      qsState = QuickSettingsState::TAB_FOCUSED;
      requestUpdate();
    }
  }
}

void EpubReaderActivity::applyQuickSettings() {
  // Commit local modifications to the global CrossPointSettings Singleton
  SETTINGS.fontFamily = tempSettings.fontFamily;
  SETTINGS.fontSize = tempSettings.fontSize;
  SETTINGS.lineSpacing = tempSettings.lineSpacing;
  SETTINGS.screenMargin = tempSettings.screenMargin;
  SETTINGS.paragraphAlignment = tempSettings.paragraphAlignment;
  SETTINGS.embeddedStyle = tempSettings.embeddedStyle;
  SETTINGS.hyphenationEnabled = tempSettings.hyphenationEnabled;
  SETTINGS.extraParagraphSpacing = tempSettings.extraParagraphSpacing;
  SETTINGS.textAntiAliasing = tempSettings.textAntiAliasing;
  SETTINGS.sideButtonLayout = tempSettings.sideButtonLayout;
  SETTINGS.longPressChapterSkip = tempSettings.longPressChapterSkip;
  SETTINGS.shortPwrBtn = tempSettings.shortPwrBtn;

  SETTINGS.saveToFile();

  // Close the overlay BEFORE the blocking reflow operation
  qsState = QuickSettingsState::CLOSED;

  {
    RenderLock lock(*this);

    cachedSpineIndex = currentSpineIndex;
    if (section) {
      cachedChapterTotalPageCount = section->pageCount;
    }
    section.reset();
    pagesUntilFullRefresh = 0;
  }

  // Trigger a full screen render to apply the heavy layout changes
  requestUpdate();
}

void EpubReaderActivity::renderQuickSettingsOverlay() {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();

  // tallewr menu (300px)
  const int overlayH = 315;
  const int overlayY = h - overlayH;

  // Blank out the background
  renderer.fillRect(0, overlayY, w, overlayH, false);
  // Top Border
  renderer.fillRect(0, overlayY, w, 2, true);

  // --- DRAW TABS ---
  const int tabY = overlayY + 15;

  const char* t1 = tr(STR_CAT_READER);
  const char* t2 = tr(STR_CAT_CONTROLS);

  int t1W = renderer.getTextWidth(UI_12_FONT_ID, t1, EpdFontFamily::BOLD);
  int t2W = renderer.getTextWidth(UI_12_FONT_ID, t2, EpdFontFamily::BOLD);

  int t1X = (w / 4) - (t1W / 2);
  int t2X = (w * 3 / 4) - (t2W / 2);

  bool isReaderActive = (qsSelectedTab == 0);

  if (qsState == QuickSettingsState::TAB_FOCUSED && isReaderActive) {
    renderer.fillRect(t1X - 15, tabY - 3, t1W + 30, 35, true);
  } else if (qsState == QuickSettingsState::TAB_FOCUSED && !isReaderActive) {
    renderer.fillRect(t2X - 15, tabY - 3, t2W + 30, 35, true);
  }

  renderer.drawText(UI_12_FONT_ID, t1X, tabY, t1, !(qsState == QuickSettingsState::TAB_FOCUSED && isReaderActive),
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, t2X, tabY, t2, !(qsState == QuickSettingsState::TAB_FOCUSED && !isReaderActive),
                    EpdFontFamily::BOLD);

  // Tab Separator Line
  renderer.fillRect(0, tabY + 30, w, 1, true);

  // --- DRAW ITEMS ---
  const int listY = tabY + 45;
  // rows (38px) & 5 items ---
  const int rowH = 38;
  const int maxVisible = 5;

  int itemCount = getQsItemCount(qsSelectedTab);
  int endIdx = std::min(qsScrollOffset + maxVisible, itemCount);

  char valBuf[32];
  char finalValBuf[64];

  for (int i = qsScrollOffset; i < endIdx; i++) {
    int rowY = listY + ((i - qsScrollOffset) * rowH);
    bool isFocused = (qsState == QuickSettingsState::ITEM_FOCUSED && qsSelectedItem == i);

    if (isFocused) {
      renderer.fillRect(10, rowY - 5, w - 20, rowH - 4, true);
    }

    renderer.drawText(UI_10_FONT_ID, 25, rowY, getQsItemName(qsSelectedTab, i), !isFocused);

    const char* valStr = getQsItemValue(qsSelectedTab, i, valBuf, sizeof(valBuf));

    if (isFocused) {
      snprintf(finalValBuf, sizeof(finalValBuf), "<  %s  >", valStr);
    } else {
      snprintf(finalValBuf, sizeof(finalValBuf), "%s", valStr);
    }

    int finalValW = renderer.getTextWidth(UI_10_FONT_ID, finalValBuf);

    renderer.drawText(UI_10_FONT_ID, w - finalValW - 25, rowY, finalValBuf, !isFocused,
                      isFocused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  // --- DRAW BUTTON HINTS ---
  const char* hintConfirm = (qsState == QuickSettingsState::ITEM_FOCUSED) ? tr(STR_CONFIRM) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), hintConfirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}