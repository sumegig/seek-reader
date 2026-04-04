#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstring>

#include "Logging.h"

static constexpr uint8_t STATS_MAX_BOOK_ENTRIES = 9;
static constexpr uint32_t STATS_MIN_SESSION_MS = 5UL * 60UL * 1000UL;
static constexpr uint8_t STATS_SESSION_RING_SIZE = 7;
static constexpr uint8_t STATS_FILE_VERSION = 5;  // UPDATE: bumped from 4 to 5
static constexpr const char* STATS_FILE_PATH = "/.crosspoint/stats.bin";

// -----------------------------------------------------------------------
// Per-book stats record
// -----------------------------------------------------------------------
struct BookStatEntry {
  char cacheKey[64];       // epub->getCachePath()
  char title[64];          // epub->getTitle()
  char author[64];         // NEW: epub->getAuthor()
  char bookPath[128];      // epub->getPath()
  char thumbBmpPath[128];  // epub->getThumbBmpPath()
  uint32_t totalReadingMs;
  uint32_t sessionCount;
  uint32_t totalPagesRead;  // NEW: Total pages read in this specific book
  uint8_t progressPercent;
  uint8_t _pad[3];  // RISC-V 4-byte padding
};
static_assert(sizeof(BookStatEntry) == 464, "BookStatEntry layout changed — bump STATS_FILE_VERSION");

// -----------------------------------------------------------------------
// Global stats record
// -----------------------------------------------------------------------
struct GlobalStats {
  uint8_t version;
  uint8_t sessionRingHead;
  uint8_t sessionRingCount;
  uint8_t bookCount;
  uint16_t totalBooksFinished;  // NEW: Total books finished on this device
  uint8_t _pad[2];              // NEW: RISC-V 4-byte padding
  uint32_t totalReadingMs;
  uint32_t totalSessionCount;
  uint32_t sessionRing[STATS_SESSION_RING_SIZE];
};
static_assert(sizeof(GlobalStats) == 44, "GlobalStats layout changed — bump STATS_FILE_VERSION");

// -----------------------------------------------------------------------
// ReadingStatsManager singleton
// -----------------------------------------------------------------------
class ReadingStatsManager {
 public:
  static ReadingStatsManager& getInstance() {
    static ReadingStatsManager instance;
    return instance;
  }

  bool load();

  void beginSession(const char* cacheKey, const char* title, const char* author, const char* bookPath,
                    const char* thumbBmpPath, uint8_t progressPercent);

  void endSession(uint8_t progressPercent, uint32_t sessionPagesTurned);

  const GlobalStats& getGlobal() const { return global; }
  uint8_t getBookCount() const { return global.bookCount; }
  const BookStatEntry& getBook(uint8_t index) const { return books[index]; }
  uint32_t getLast7SessionsMs() const;
  uint8_t getLast7SessionCount() const { return global.sessionRingCount; }

 private:
  ReadingStatsManager() = default;

  bool save();
  int findBook(const char* cacheKey) const;
  void bringBookToFront(uint8_t index);
  void sortByProgress();

  GlobalStats global{};
  BookStatEntry books[STATS_MAX_BOOK_ENTRIES]{};

  TickType_t sessionStartTick = 0;
  bool sessionActive = false;
  uint8_t sessionBookIndex = 0xFF;
};

#define StatsManager ReadingStatsManager::getInstance()