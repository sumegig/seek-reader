#pragma once
#include <functional>
#include <vector>

#include "../Activity.h"
#include "./FileBrowserActivity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  // --- Shared State ---
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool coverBufferStored = false;
  uint8_t* coverBuffer = nullptr;

  // --- Original 1D Theme State ---
  int selectorIndex = 0;
  bool coverRendered = false;
  std::vector<RecentBook> recentBooks;

  // --- New Recent6 (2D) Theme State ---
  int carouselSelectedIndex = 0;
  bool carouselFocused = true;
  int menuSelectedTileIndex = 0;
  bool row1Rendered = false;
  bool row1Stored = false;
  bool row2Rendered = false;
  bool row2Stored = false;
  int totalRecentBooks = 0;
  std::vector<RecentBook> recentBooksRow1;
  std::vector<RecentBook> recentBooksRow2;

  // --- Methods ---
  bool isRecent6Theme() const;
  int getMenuItemCount() const;
  
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onAppsOpen(); // Every theme uses this from now on Instead of File transfer and OPDS

  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

  // Navigáció (Recent6)
  void focusCarousel();
  void focusMenu();
  void launchSelectedActivity();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Home", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};