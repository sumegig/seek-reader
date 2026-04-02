#include "AppsActivity.h"

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/stats/StatsActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

void AppsActivity::onEnter() {
  Activity::onEnter();

  enterTime = xTaskGetTickCount();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;
  selectorIndex = 0;

  requestUpdate();
}

void AppsActivity::onExit() { Activity::onExit(); }

void AppsActivity::loop() {
  // COOLDOWN: Ignore buttons for 500ms to resolve ping-pong!
  if (xTaskGetTickCount() - enterTime < pdMS_TO_TICKS(500)) {
    return;
  }

  // Go back to Home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }

  // Calculate dynamic menu size based on OPDS setting
  const int menuCount = hasOpdsUrl ? 3 : 2;

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
    const int statsIdx = idx++;
    const int fileTransferIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;

    // Callback that runs when we return to this Apps menu from any sub-activity
    auto onSubActivityReturned = [this](const ActivityResult& result) {
      // Reset the cooldown timer to prevent the bouncing/ping-pong effect 
      // of the Back button being pressed during the transition.
      enterTime = xTaskGetTickCount();
    };

    // Navigate to the selected app, using startActivityForResult so we know when we return
    if (selectorIndex == statsIdx) {
      startActivityForResult(std::make_unique<StatsActivity>(renderer, mappedInput), onSubActivityReturned);
    } else if (selectorIndex == fileTransferIdx) {
      startActivityForResult(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput), onSubActivityReturned);
    } else if (selectorIndex == opdsLibraryIdx) {
      startActivityForResult(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput), onSubActivityReturned);
    }
  }
}

void AppsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Standard header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_APPS));

  // Build menu items
  std::vector<const char*> menuItems = {tr(STR_STATS_TITLE), tr(STR_FILE_TRANSFER)};
  // Reusing the Library/Transfer icons for the apps menu
  std::vector<UIIcon> menuIcons = {Library, Transfer};

  if (hasOpdsUrl) {
    menuItems.push_back(tr(STR_OPDS_BROWSER));
    menuIcons.push_back(Library);
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawButtonMenu(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(menuItems.size()), selectorIndex,
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}