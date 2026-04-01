#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstring>

#include "Logging.h"

static constexpr uint8_t  STATS_MAX_BOOK_ENTRIES  = 5;
static constexpr uint32_t STATS_MIN_SESSION_MS    = 5UL * 60UL * 1000UL;
static constexpr uint8_t  STATS_SESSION_RING_SIZE = 7;
static constexpr uint8_t  STATS_FILE_VERSION      = 3;
static constexpr const char* STATS_FILE_PATH      = "/.crosspoint/stats.bin";

// -----------------------------------------------------------------------
// Per-book stats record
// -----------------------------------------------------------------------
struct BookStatEntry {
    char     cacheKey[64];       // epub->getCachePath()
    char     title[64];          // epub->getTitle()
    char     bookPath[128];      // epub->getPath() — for opening book
    char     thumbBmpPath[128];  // epub->getThumbBmpPath() — contains [HEIGHT]
    uint32_t totalReadingMs;
    uint32_t sessionCount;
    uint8_t  progressPercent;
    uint8_t  _pad[3];
};
static_assert(sizeof(BookStatEntry) == 396,
              "BookStatEntry layout changed — bump STATS_FILE_VERSION");

// -----------------------------------------------------------------------
// Global stats record
// -----------------------------------------------------------------------
struct GlobalStats {
    uint8_t  version;
    uint8_t  sessionRingHead;
    uint8_t  sessionRingCount;
    uint8_t  bookCount;
    uint32_t totalReadingMs;
    uint32_t totalSessionCount;
    uint32_t sessionRing[STATS_SESSION_RING_SIZE];
};
static_assert(sizeof(GlobalStats) == 40,
              "GlobalStats layout changed — bump STATS_FILE_VERSION");

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

    void beginSession(const char* cacheKey, const char* title,
                      const char* bookPath, const char* thumbBmpPath,
                      uint8_t progressPercent);

    void endSession(uint8_t progressPercent);

    const GlobalStats&   getGlobal()              const { return global; }
    uint8_t              getBookCount()            const { return global.bookCount; }
    const BookStatEntry& getBook(uint8_t index)    const { return books[index]; }
    uint32_t             getLast7SessionsMs()       const;
    uint8_t              getLast7SessionCount()     const { return global.sessionRingCount; }

 private:
    ReadingStatsManager() = default;

    bool save();
    int  findBook(const char* cacheKey) const;
    void bringBookToFront(uint8_t index);
    void sortByProgress();

    GlobalStats   global{};
    BookStatEntry books[STATS_MAX_BOOK_ENTRIES]{};

    TickType_t sessionStartTick  = 0;
    bool       sessionActive     = false;
    uint8_t    sessionBookIndex  = 0xFF;
};

#define StatsManager ReadingStatsManager::getInstance()