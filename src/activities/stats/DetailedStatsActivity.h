#pragma once

#include <cstdint>

#include "activities/Activity.h"

class DetailedStatsActivity final : public Activity {
 public:
  DetailedStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t bookIndex);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void renderDetailedGrid() const;
  void drawCoverPlaceholder(int x, int y, int w, int h) const;

  uint8_t _bookIndex;
};