#pragma once

#include <string>
#include <vector>

#include "I18n.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    ROTATE_SCREEN,
    AUTO_PAGE_TURN,
    GO_TO_PERCENT,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    LOOKUP,           // Added for Dictionary functionality
    LOOKED_UP_WORDS,  // Added for Dictionary history
    QUICK_SETTINGS    // <-- Triggers the in-activity overlay
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
    std::string customLabel = "";  // Fallback for labels without StrId (like "Lookup")
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, const bool hasDictionary,
                                  const bool hasLookupHistory);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<MenuItem> menuItems;
  std::string title;
  uint8_t pendingOrientation;
  int currentPage;
  int totalPages;
  int bookProgressPercent;
  int selectedIndex = 0;
  uint8_t selectedPageTurnOption = 0;

  ButtonNavigator buttonNavigator;

  std::vector<MenuItem> buildMenuItems(bool hasFootnotes, bool hasDictionary, bool hasLookupHistory);
};