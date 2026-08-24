#include "OtaTaskManager.h"

#include <Logging.h>
#include <freertos/semphr.h>

#include "esp_https_ota.h"
#include "esp_wifi.h"

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}


OtaTaskManager::OtaTaskManager() = default;

OtaTaskManager::~OtaTaskManager() {
  if (taskHandle != nullptr) {
    vTaskDelete(taskHandle);
    taskHandle = nullptr;
  }
}

void OtaTaskManager::startDownload(const std::string& url, size_t size, ProgressCallback onProgress,
                                    CompletionCallback onComplete) {
  if (taskHandle != nullptr) {
    LOG_ERR("OTA", "OTA task already running");
    return;
  }

  downloadUrl = url;
  totalSize = size;
  processedSize = 0;
  progressCallback = onProgress;
  completionCallback = onComplete;
  cancelRequested = false;
  state = DOWNLOADING;

  const char taskName[] = "OtaTask";
  const uint32_t stackSize = 4096;  // Enough for HTTP operations
  const UBaseType_t priority = 2;   // Higher than main task (priority 1)

  if (xTaskCreate(&OtaTaskManager::taskTrampoline, taskName, stackSize, this, priority, &taskHandle) != pdPASS) {
    LOG_ERR("OTA", "Failed to create OTA task");
    state = FAILED;
    if (completionCallback) {
      completionCallback(false);
    }
    return;
  }

  LOG_INF("OTA", "OTA task started");
}

void OtaTaskManager::cancel() {
  cancelRequested = true;
  LOG_DBG("OTA", "OTA download cancelled");
}

void OtaTaskManager::taskTrampoline(void* arg) {
  auto* manager = static_cast<OtaTaskManager*>(arg);
  manager->taskRun();
  vTaskDelete(nullptr);  // Delete self when done
}

void OtaTaskManager::taskRun() {
  esp_http_client_config_t client_config = {
      .url = downloadUrl.c_str(),
      .timeout_ms = 15000,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = nullptr,
  };

  esp_https_ota_handle_t ota_handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);

  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_begin failed: %s", esp_err_to_name(err));
    state = FAILED;
    if (completionCallback) {
      completionCallback(false);
    }
    return;
  }

  // Download loop - runs in background task, doesn't block main activity loop
  while (!cancelRequested && err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);

    // Call progress callback without blocking
    if (progressCallback) {
      progressCallback(processedSize, totalSize);
    }

    // Give other tasks a chance to run
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (cancelRequested) {
    LOG_INF("OTA", "OTA download cancelled");
    esp_https_ota_finish(ota_handle);
    state = FAILED;
    if (completionCallback) {
      completionCallback(false);
    }
    return;
  }

  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform failed: %s", esp_err_to_name(err));
    esp_https_ota_finish(ota_handle);
    state = FAILED;
    if (completionCallback) {
      completionCallback(false);
    }
    return;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "Incomplete OTA data received");
    esp_https_ota_finish(ota_handle);
    state = FAILED;
    if (completionCallback) {
      completionCallback(false);
    }
    return;
  }

  err = esp_https_ota_finish(ota_handle);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish failed: %s", esp_err_to_name(err));
    state = FAILED;
    if (completionCallback) {
      completionCallback(false);
    }
    return;
  }

  LOG_INF("OTA", "OTA download complete");
  state = COMPLETE;
  if (completionCallback) {
    completionCallback(true);
  }
}
