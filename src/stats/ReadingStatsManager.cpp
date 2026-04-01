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

    uint8_t rawGlobal[sizeof(GlobalStats)];
    if (f.read(rawGlobal, sizeof(GlobalStats)) != sizeof(GlobalStats)) {
        LOG_ERR("STATS", "Corrupt stats file (global header), resetting");
        f.close();
        global = GlobalStats{};
        global.version = STATS_FILE_VERSION;
        memset(books, 0, sizeof(books));
        return false;
    }
    memcpy(&global, rawGlobal, sizeof(GlobalStats));

    if (global.version != STATS_FILE_VERSION) {
        LOG_DBG("STATS", "Stats version mismatch (%d != %d), resetting",
                global.version, STATS_FILE_VERSION);
        f.close();
        global = GlobalStats{};
        global.version = STATS_FILE_VERSION;
        memset(books, 0, sizeof(books));
        return true;
    }

    if (global.bookCount > STATS_MAX_BOOK_ENTRIES) {
        global.bookCount = STATS_MAX_BOOK_ENTRIES;
    }

    memset(books, 0, sizeof(books));
    for (uint8_t i = 0; i < global.bookCount; ++i) {
        uint8_t rawBook[sizeof(BookStatEntry)];
        if (f.read(rawBook, sizeof(BookStatEntry)) != sizeof(BookStatEntry)) {
            LOG_ERR("STATS", "Corrupt stats file (book %d), truncating", i);
            global.bookCount = i;
            break;
        }
        memcpy(&books[i], rawBook, sizeof(BookStatEntry));
    }

    f.close();
    LOG_DBG("STATS", "Loaded stats: %d books, %lu total ms",
            global.bookCount, (unsigned long)global.totalReadingMs);
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
        if (strncmp(books[i].cacheKey, cacheKey,
                    sizeof(books[i].cacheKey)) == 0) {
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
    // Insertion sort descending by progressPercent — max 5 elements
    for (uint8_t i = 1; i < global.bookCount; ++i) {
        BookStatEntry tmp;
        memcpy(&tmp, &books[i], sizeof(BookStatEntry));
        int8_t j = static_cast<int8_t>(i) - 1;
        while (j >= 0 &&
               books[j].progressPercent < tmp.progressPercent) {
            memcpy(&books[j + 1], &books[j], sizeof(BookStatEntry));
            j--;
        }
        memcpy(&books[j + 1], &tmp, sizeof(BookStatEntry));
    }
}

void ReadingStatsManager::beginSession(const char* cacheKey,
                                        const char* title,
                                        const char* bookPath,
                                        const char* thumbBmpPath,
                                        uint8_t     progressPercent) {
    sessionStartTick = xTaskGetTickCount();
    sessionActive    = true;

    int idx = findBook(cacheKey);
    if (idx == -1) {
        if (global.bookCount < STATS_MAX_BOOK_ENTRIES) {
            global.bookCount++;
        }
        for (uint8_t i = global.bookCount - 1; i > 0; --i) {
            memcpy(&books[i], &books[i - 1], sizeof(BookStatEntry));
        }
        memset(&books[0], 0, sizeof(BookStatEntry));
        strncpy(books[0].cacheKey,     cacheKey,     sizeof(books[0].cacheKey)     - 1);
        strncpy(books[0].title,        title,        sizeof(books[0].title)        - 1);
        strncpy(books[0].bookPath,     bookPath,     sizeof(books[0].bookPath)     - 1);
        strncpy(books[0].thumbBmpPath, thumbBmpPath, sizeof(books[0].thumbBmpPath) - 1);
        books[0].progressPercent = progressPercent;
    } else {
        bringBookToFront(static_cast<uint8_t>(idx));
        strncpy(books[0].bookPath,     bookPath,     sizeof(books[0].bookPath)     - 1);
        strncpy(books[0].thumbBmpPath, thumbBmpPath, sizeof(books[0].thumbBmpPath) - 1);
        books[0].progressPercent = progressPercent;
    }
    sessionBookIndex = 0;
}

void ReadingStatsManager::endSession(uint8_t progressPercent) {
    if (!sessionActive) return;
    sessionActive = false;

    const TickType_t endTick   = xTaskGetTickCount();
    const uint32_t   elapsedMs = (endTick - sessionStartTick) * portTICK_PERIOD_MS;

    const bool progressChanged =
        (sessionBookIndex < global.bookCount) &&
        (books[sessionBookIndex].progressPercent != progressPercent);

    if (sessionBookIndex < global.bookCount) {
        books[sessionBookIndex].progressPercent = progressPercent;
    }

    const bool longEnough = (elapsedMs >= STATS_MIN_SESSION_MS);

    if (longEnough) {
        if (sessionBookIndex < global.bookCount) {
            books[sessionBookIndex].totalReadingMs += elapsedMs;
            books[sessionBookIndex].sessionCount++;
        }
        global.totalReadingMs    += elapsedMs;
        global.totalSessionCount++;
        global.sessionRing[global.sessionRingHead] = elapsedMs;
        global.sessionRingHead =
            (global.sessionRingHead + 1) % STATS_SESSION_RING_SIZE;
        if (global.sessionRingCount < STATS_SESSION_RING_SIZE) {
            global.sessionRingCount++;
        }
    }

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