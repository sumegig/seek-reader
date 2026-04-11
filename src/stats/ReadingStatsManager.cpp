#include "stats/ReadingStatsManager.h"

#include <HalStorage.h>

#include <cstring>

bool ReadingStatsManager::load() {
  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) {
    LOG_DBG("STATS", "No stats file found, starting fresh");
    global = GlobalStats{};
    global.version = STATS_FILE_VERSION;
    memset(books, 0, sizeof(books));
    return true;
  }

  // Peek version byte to determine migration path
  uint8_t fileVersion;
  f.read(&fileVersion, 1);
  f.seek(0);  // Reset file pointer

  if (fileVersion < STATS_FILE_VERSION) {
    LOG_INF("STATS", "Old version detected (%d), migrating to %d...", fileVersion, STATS_FILE_VERSION);

    // GlobalStats: v4 = 40 bytes, v5+ = 44 bytes
    size_t globalSize = (fileVersion == 4) ? 40 : 44;
    f.read(&global, globalSize);
    global.version = STATS_FILE_VERSION;  // Perform in-memory upgrade

    // BookStatEntry: v4 = 396 bytes, v5 = 464 bytes
    size_t oldEntrySize = (fileVersion == 4) ? 396 : 464;
    memset(books, 0, sizeof(books));

    for (uint8_t i = 0; i < global.bookCount; ++i) {
      // Read based on historical record size
      f.read(&books[i], oldEntrySize);
      // books[i].lastSessionMs is initialized to 0 by memset
    }
    f.close();
    save();  // Persist migrated data immediately
    return true;
  }

  // Standard v6 load
  f.read(&global, sizeof(GlobalStats));
  for (uint8_t i = 0; i < global.bookCount; ++i) {
    f.read(&books[i], sizeof(BookStatEntry));
  }

  f.close();
  return true;
}

bool ReadingStatsManager::save() {
  FsFile f;
  if (!Storage.openFileForWrite("STATS", STATS_FILE_PATH, f)) {
    LOG_ERR("STATS", "Could not open stats file for write");
    return false;
  }

  uint8_t rawGlobal[sizeof(GlobalStats)];
  memcpy(rawGlobal, &global, sizeof(GlobalStats));
  f.write(rawGlobal, sizeof(GlobalStats));

  for (uint8_t i = 0; i < global.bookCount; ++i) {
    uint8_t rawBook[sizeof(BookStatEntry)];
    memcpy(rawBook, &books[i], sizeof(BookStatEntry));
    f.write(rawBook, sizeof(BookStatEntry));
  }

  f.close();
  LOG_DBG("STATS", "Stats saved");
  return true;
}

int ReadingStatsManager::findBook(const char* cacheKey) const {
  for (uint8_t i = 0; i < global.bookCount; ++i) {
    if (strncmp(books[i].cacheKey, cacheKey, sizeof(books[i].cacheKey)) == 0) {
      return i;
    }
  }
  return -1;
}

void ReadingStatsManager::bringBookToFront(uint8_t index) {
  if (index == 0) return;
  BookStatEntry tmp;
  memcpy(&tmp, &books[index], sizeof(BookStatEntry));
  for (uint8_t i = index; i > 0; --i) {
    memcpy(&books[i], &books[i - 1], sizeof(BookStatEntry));
  }
  memcpy(&books[0], &tmp, sizeof(BookStatEntry));
}

void ReadingStatsManager::sortByProgress() {
  // Insertion sort descending by progressPercent — max 9 elements
  for (uint8_t i = 1; i < global.bookCount; ++i) {
    BookStatEntry tmp;
    memcpy(&tmp, &books[i], sizeof(BookStatEntry));
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && books[j].progressPercent < tmp.progressPercent) {
      memcpy(&books[j + 1], &books[j], sizeof(BookStatEntry));
      j--;
    }
    memcpy(&books[j + 1], &tmp, sizeof(BookStatEntry));
  }
}

void ReadingStatsManager::beginSession(const char* cacheKey, const char* title, const char* author,
                                       const char* bookPath, const char* thumbBmpPath, uint8_t progressPercent) {
  sessionStartTick = xTaskGetTickCount();
  sessionActive = true;

  int idx = findBook(cacheKey);
  if (idx == -1) {
    if (global.bookCount < STATS_MAX_BOOK_ENTRIES) {
      global.bookCount++;
    }
    for (uint8_t i = global.bookCount - 1; i > 0; --i) {
      memcpy(&books[i], &books[i - 1], sizeof(BookStatEntry));
    }
    memset(&books[0], 0, sizeof(BookStatEntry));
    strncpy(books[0].cacheKey, cacheKey, sizeof(books[0].cacheKey) - 1);
    strncpy(books[0].title, title, sizeof(books[0].title) - 1);
    strncpy(books[0].author, author, sizeof(books[0].author) - 1);
    strncpy(books[0].bookPath, bookPath, sizeof(books[0].bookPath) - 1);
    strncpy(books[0].thumbBmpPath, thumbBmpPath, sizeof(books[0].thumbBmpPath) - 1);
    books[0].progressPercent = progressPercent;
  } else {
    bringBookToFront(static_cast<uint8_t>(idx));
    strncpy(books[0].bookPath, bookPath, sizeof(books[0].bookPath) - 1);
    strncpy(books[0].thumbBmpPath, thumbBmpPath, sizeof(books[0].thumbBmpPath) - 1);
    books[0].progressPercent = progressPercent;
  }
  sessionBookIndex = 0;
}

void ReadingStatsManager::endSession(uint8_t progressPercent, uint32_t sessionPagesTurned) {
  if (!sessionActive) return;
  sessionActive = false;

  const uint32_t elapsedMs = (xTaskGetTickCount() - sessionStartTick) * portTICK_PERIOD_MS;

  if (sessionBookIndex < global.bookCount) {
    if (progressPercent == 100 && books[sessionBookIndex].progressPercent < 100) {
      global.totalBooksFinished++;
    }
    books[sessionBookIndex].progressPercent = progressPercent;
    // Always store the duration of the most recent session
    books[sessionBookIndex].lastSessionMs = elapsedMs;
  }

  const bool longEnough = (elapsedMs >= STATS_MIN_SESSION_MS);

  if (longEnough) {
    if (sessionBookIndex < global.bookCount) {
      books[sessionBookIndex].totalReadingMs += elapsedMs;
      books[sessionBookIndex].sessionCount++;
      books[sessionBookIndex].totalPagesRead += sessionPagesTurned;  // Tracking pages turned
    }
    global.totalReadingMs += elapsedMs;
    global.totalSessionCount++;
    global.sessionRing[global.sessionRingHead] = elapsedMs;
    global.sessionRingHead = (global.sessionRingHead + 1) % STATS_SESSION_RING_SIZE;
    if (global.sessionRingCount < STATS_SESSION_RING_SIZE) {
      global.sessionRingCount++;
    }
  }

  // Save if time threshold reached or progress moved
  const bool progressChanged = (sessionBookIndex < global.bookCount);
  if (longEnough || progressChanged) {
    sortByProgress();
    save();
  }
}

uint32_t ReadingStatsManager::getLast7SessionsMs() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < global.sessionRingCount; ++i) {
    total += global.sessionRing[i];
  }
  return total;
}