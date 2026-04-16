#pragma once

#include "activities/Activity.h"
#include "stats/ReadingStatsManager.h"

class StatsActivity final : public Activity {
 public:
  explicit StatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Stats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void renderTopPanel(int panelY, int panelH, int screenW) const;
  void renderBookPanel(int panelY, int panelH, int screenW) const;
  void renderBookRow(int rowX, int rowY, int rowW, int rowH, const BookStatEntry& book, bool selected) const;
  void drawCoverPlaceholder(int x, int y, int w, int h) const;
  bool loadAndDrawCover(int x, int y, int w, int h, const BookStatEntry& book) const;
  bool showingFinished = false;  // NEW: Toggle between Reading and Finished views

  static void formatDuration(char* buf, size_t bufLen, uint32_t ms);
  static void formatPercent(char* buf, size_t bufLen, uint8_t percent);
  uint8_t getVisibleBookCount() const;

  int selectedBookIndex = 0;
};