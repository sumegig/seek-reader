#include "DictionaryDefinitionActivity.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"
#include "components/UITheme.h"
#include "CrossPointSettings.h"
#include <sstream>

DictionaryDefinitionActivity::DictionaryDefinitionActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, 
    std::string word, std::string cachePath, int fontId)
    : Activity("DictionaryDefinition", renderer, mappedInput),
      targetWord(std::move(word)),
      cachePath(std::move(cachePath)),
      fontId(fontId) {}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  requestUpdate(); 
  performLookup();
}

void DictionaryDefinitionActivity::performLookup() {
  definition = Dictionary::lookup(targetWord);

  if (definition.empty()) {
    auto stems = Dictionary::getStemVariants(targetWord);
    for (const auto& stem : stems) {
      definition = Dictionary::lookup(stem);
      if (!definition.empty()) {
        targetWord = stem; 
        break;
      }
    }
  }

  if (definition.empty()) {
    notFound = true;
  } else {
    LookupHistory::addWord(cachePath, targetWord);
    wrapText();
  }

  isLoading = false;
  requestUpdate();
}

void DictionaryDefinitionActivity::wrapText() {
  wrappedLines.clear();
  if (definition.empty()) return;

  const int margin = SETTINGS.screenMargin * 2;
  const int maxWidth = renderer.getDisplayWidth() - (margin * 2);
  
  std::string currentLine;
  std::string currentWord;
  
  for (char c : definition) {
    if (c == ' ' || c == '\n') {
      int lineWidth = renderer.getTextWidth(fontId, (currentLine + currentWord).c_str());
      if (lineWidth > maxWidth && !currentLine.empty()) {
        wrappedLines.push_back(currentLine);
        currentLine = currentWord + " ";
      } else {
        currentLine += currentWord + " ";
      }
      currentWord.clear();
      
      if (c == '\n') {
        wrappedLines.push_back(currentLine);
        currentLine.clear();
      }
    } else {
      currentWord += c;
    }
  }
  
  if (!currentWord.empty() || !currentLine.empty()) {
    wrappedLines.push_back(currentLine + currentWord);
  }

  lineHeight = renderer.getLineHeight(fontId);
  const int headerHeight = lineHeight * 3; 
  const int availableHeight = renderer.getDisplayHeight() - headerHeight - 40;
  
  linesPerPage = availableHeight / lineHeight;
  maxScroll = std::max(0, static_cast<int>(wrappedLines.size()) - linesPerPage);
}

void DictionaryDefinitionActivity::loop() {
  if (isLoading) return;

  bool needsUpdate = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (scrollOffset > 0) {
      scrollOffset = std::max(0, scrollOffset - linesPerPage + 1);
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (scrollOffset < maxScroll) {
      scrollOffset = std::min(maxScroll, scrollOffset + linesPerPage - 1);
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult result;
    setResult(std::move(result));
    finish();
  }

  if (needsUpdate) requestUpdate();
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  
  const int margin = SETTINGS.screenMargin * 2;
  int currentY = margin;

  if (isLoading) {
    renderer.drawText(fontId, margin, currentY, "Looking up...");
  } else if (notFound) {
    renderer.drawText(fontId, margin, currentY, ("Word not found: " + targetWord).c_str());
  } else {
    // FIX: Removed the 'false' flag to use default rendering (black text), ensuring it is always visible!
    renderer.drawText(fontId, margin, currentY, targetWord.c_str()); 
    
    // Draw a nice 3-pixel thick underline to separate the title from the definition
    int titleWidth = renderer.getTextWidth(fontId, targetWord.c_str());
    renderer.fillRect(margin, currentY + lineHeight + 2, titleWidth, 3, true);
    
    currentY += lineHeight + (margin * 2);

    int linesToDraw = std::min(linesPerPage, static_cast<int>(wrappedLines.size()) - scrollOffset);
    for (int i = 0; i < linesToDraw; ++i) {
      renderer.drawText(fontId, margin, currentY, wrappedLines[scrollOffset + i].c_str());
      currentY += lineHeight;
    }
    
    if (scrollOffset > 0) {
      renderer.drawText(fontId, renderer.getDisplayWidth() - margin - 20, margin * 2, "^");
    }
    if (scrollOffset < maxScroll) {
      renderer.drawText(fontId, renderer.getDisplayWidth() - margin - 20, renderer.getDisplayHeight() - margin * 3, "v");
    }
  }

  const auto labels = mappedInput.mapLabels("Close", "", "Scroll", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
