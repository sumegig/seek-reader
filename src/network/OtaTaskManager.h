#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <functional>
#include <string>

class OtaUpdater;

/**
 * Manages OTA updates in a dedicated background FreeRTOS task.
 * Prevents SD card locking by keeping OTA off the main activity loop.
 */
class OtaTaskManager {
 public:
  enum TaskState {
    IDLE,
    DOWNLOADING,
    COMPLETE,
    FAILED,
  };

  using ProgressCallback = std::function<void(size_t processed, size_t total)>;
  using CompletionCallback = std::function<void(bool success)>;

  OtaTaskManager();
  ~OtaTaskManager();

  /**
   * Start OTA download in background task.
   * @param url Download URL for firmware binary
   * @param size Total firmware size in bytes
   * @param onProgress Called periodically with download progress
   * @param onComplete Called when download finishes (success or failure)
   */
  void startDownload(const std::string& url, size_t size, ProgressCallback onProgress, CompletionCallback onComplete);

  /**
   * Cancel ongoing download.
   */
  void cancel();

  /**
   * Get current task state.
   */
  TaskState getState() const { return state; }

  /**
   * Get bytes downloaded so far.
   */
  size_t getProcessedSize() const { return processedSize; }

  /**
   * Get total firmware size.
   */
  size_t getTotalSize() const { return totalSize; }

 private:
  static void taskTrampoline(void* arg);
  void taskRun();

  TaskHandle_t taskHandle = nullptr;
  TaskState state = IDLE;
  size_t processedSize = 0;
  size_t totalSize = 0;
  std::string downloadUrl;
  bool cancelRequested = false;

  ProgressCallback progressCallback;
  CompletionCallback completionCallback;
};
