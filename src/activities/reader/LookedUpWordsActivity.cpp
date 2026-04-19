#include "LookedUpWordsActivity.h"

#include <I18n.h>

#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/LookupHistory.h"

LookedUpWordsActivity::LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string cachePath)
    : Activity("LookedUpWords", renderer, mappedInput), cachePath(std::move(cachePath)) {}

void LookedUpWordsActivity::onEnter() {
  Activity::onEnter();
  // We trigger an initial render to show "Loading", then load data in the loop
  isLoading = true;
  requestUpdate();
}

void LookedUpWordsActivity::loadHistory() {
  words = LookupHistory::load(cachePath);

  // Reverse the vector to show the most recent lookups at the top
  std::reverse(words.begin(), words.end());

  isLoading = false;
  requestUpdate();
}

void LookedUpWordsActivity::deleteSelectedWord() {
  if (words.empty() || selectedIndex < 0 || selectedIndex >= words.size()) return;

  LookupHistory::removeWord(cachePath, words[selectedIndex]);
  words.erase(words.begin() + selectedIndex);

  // Adjust selection to prevent out of bounds
  if (selectedIndex >= words.size() && selectedIndex > 0) {
    selectedIndex--;
  }

  // Adjust scroll offset if necessary
  if (scrollOffset > selectedIndex) {
    scrollOffset = selectedIndex;
  }
}

void LookedUpWordsActivity::loop() {
  // Postpone heavy IO until first render is done
  if (isLoading) {
    loadHistory();
    return;
  }

  if (words.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ActivityResult result;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  bool needsUpdate = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (selectedIndex > 0) {
      selectedIndex--;
      if (selectedIndex < scrollOffset) {
        scrollOffset = selectedIndex;
      }
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (selectedIndex < static_cast<int>(words.size()) - 1) {
      selectedIndex++;
      if (selectedIndex >= scrollOffset + linesPerPage) {
        scrollOffset = selectedIndex - linesPerPage + 1;
      }
      needsUpdate = true;
    }
  }

  // Handle Confirm (Short press = Lookup, Long press = Delete)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= 800) {
      // Long press: Delete word
      deleteSelectedWord();
      needsUpdate = true;
    } else {
      // Short press: Lookup word
      // Note: Re-using the same fontId as the reader settings might require passing it down,
      // but UI_12_FONT_ID works as a safe default for definition reading.
      startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, words[selectedIndex],
                                                                            cachePath, UI_12_FONT_ID),
                             [this](const ActivityResult& result) { requestUpdate(); });
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    setResult(std::move(result));
    finish();
  }

  if (needsUpdate) requestUpdate();
}

void LookedUpWordsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int margin = 20;
  int currentY = margin;

  if (isLoading) {
    renderer.drawText(UI_12_FONT_ID, margin, currentY, tr(STR_LOOKING_UP));
    renderer.displayBuffer();
    return;
  }

  // Draw Header
  renderer.drawText(UI_12_FONT_ID, margin, currentY, tr(STR_LOOKUP_HISTORY), EpdFontFamily::BOLD);
  int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.fillRect(margin, currentY + titleH + 4, renderer.getScreenWidth() - (margin * 2), 3, true);

  currentY += titleH + 15;

  if (words.empty()) {
    renderer.drawText(UI_12_FONT_ID, margin, currentY + 20, tr(STR_NO_HISTORY_FOUND));
  } else {
    int lineHeight = renderer.getLineHeight(UI_12_FONT_ID) + 10;
    const int availableHeight = renderer.getScreenHeight() - currentY - 55;  // 55px bottom boundary
    linesPerPage = availableHeight / lineHeight;

    int linesToDraw = std::min(linesPerPage, static_cast<int>(words.size()) - scrollOffset);

    for (int i = 0; i < linesToDraw; ++i) {
      int itemIndex = scrollOffset + i;
      bool isSelected = (itemIndex == selectedIndex);

      if (isSelected) {
        // Draw selection background
        renderer.fillRect(margin - 5, currentY - 2, renderer.getScreenWidth() - (margin * 2) + 10, lineHeight, true);
      }

      renderer.drawText(UI_12_FONT_ID, margin, currentY, words[itemIndex].c_str(), !isSelected);
      currentY += lineHeight;
    }
  }

  // Footer Hints
  // We assume you might need to add "Hold: Del" to your translations, using literal string for now
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_HINT_SEL_DEL), tr(STR_UP_DOWN), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}