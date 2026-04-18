#pragma once

#include <memory>
#include <string>
#include <vector>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h> // Added for TextBlock definition
#include "activities/Activity.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        std::string cachePath, uint8_t orientation,
                                        std::string nextPageFirstWord = "");

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct WordInfo {
    std::string text;
    std::string lookupText;
    int16_t screenX;
    int16_t screenY;
    int16_t width;
    int16_t row;
    int continuationIndex;
    int continuationOf;

    WordInfo(const std::string& t, int16_t x, int16_t y, int16_t w, int16_t r)
        : text(t), lookupText(t), screenX(x), screenY(y), width(w), row(r), continuationIndex(-1), continuationOf(-1) {}
  };

  struct Row {
    int16_t yPos;
    std::vector<int> wordIndices;
  };

  std::unique_ptr<Page> page;
  int fontId;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  uint8_t orientation;
  std::string nextPageFirstWord;

  std::vector<WordInfo> words;
  std::vector<Row> rows;
  int currentRow = 0;
  int currentWordInRow = 0;

  void extractWords();
  void mergeHyphenatedWords();
  int findClosestWordIndexInRow(int rowIndex, int targetX) const;
};