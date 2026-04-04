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
  
  // Dual-focus state
  int carouselSelectedIndex = 0;
  bool carouselFocused = true;
  int menuSelectedTileIndex = 0; // Most már csak 0-2 (3 opció)

  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  
  bool coverBufferStored = false;
  uint8_t* coverBuffer = nullptr;
  
  // Külön változók a két sor renderelésének követéséhez
  bool row1Rendered = false;
  bool row1Stored = false;
  bool row2Rendered = false;
  bool row2Stored = false;

  // A könyveket memóriabiztonsági okokból két sorra bontjuk
  std::vector<RecentBook> recentBooksRow1;
  std::vector<RecentBook> recentBooksRow2;
  int totalRecentBooks = 0;

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onSettingsOpen();
  void onAppsOpen();

  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

  // Navigáció
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