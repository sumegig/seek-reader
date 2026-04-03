#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "Activity.h"
#include "util/ButtonNavigator.h"

class AppsActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool hasOpdsUrl = false;

  TickType_t enterTime = 0;  // filetransfer to apps bug fix  // TODO: resolve later if possible

 public:
  explicit AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Apps", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};