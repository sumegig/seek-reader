#pragma once

#include <functional>
#include <memory>
#include <string>

class OtaTaskManager;

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  bool render = false;
  std::unique_ptr<OtaTaskManager> taskManager;

  // Callbacks from background task
  void onDownloadProgress(size_t processed, size_t total);
  void onDownloadComplete(bool success);

 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    TASK_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }
  size_t getProcessedSize() const { return processedSize; }
  size_t getTotalSize() const { return totalSize; }
  bool getRender() const { return render; }
  bool isDownloading() const;

  OtaUpdater();
  ~OtaUpdater();
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  /**
   * Start async OTA download in background task.
   * Does NOT block - returns immediately after starting download.
   * Use isDownloading() and getProcessedSize() to poll progress.
   */
  OtaUpdaterError installUpdateAsync();

  /**
   * DEPRECATED: Old blocking OTA method. Use installUpdateAsync() instead.
   */
  OtaUpdaterError installUpdate();

  /**
   * Cancel ongoing download.
   */
  void cancelDownload();
};
