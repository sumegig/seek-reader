#include "ProgressMapper.h"

#include <Logging.h>

#include <cmath>

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 0) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages);
  }

  // Calculate overall book progress (0.0-1.0)
  result.percentage = epub->calculateProgress(pos.spineIndex, intraSpineProgress);

  // Generate XPath with estimated paragraph position based on page
  result.xpath = generateXPath(pos.spineIndex, pos.pageNumber, pos.totalPages);

  // Get chapter info for logging
  const int tocIndex = epub->getTocIndexForSpineIndex(pos.spineIndex);
  const std::string chapterName = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "unknown";

  LOG_DBG("ProgressMapper", "CrossPoint -> KOReader: chapter='%s', page=%d/%d -> %.2f%% at %s", chapterName.c_str(),
          pos.pageNumber, pos.totalPages, result.percentage * 100, result.xpath.c_str());

  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = 0;

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return result;
  }

  // 1. Extract exact spine (chapter) index from KOReader's XPath
  bool spineFound = false;
  int parsedSpineIndex = -1;
  if (sscanf(koPos.xpath.c_str(), "/body/DocFragment[%d]", &parsedSpineIndex) == 1) {
    parsedSpineIndex -= 1;  // KOReader XPath is 1-based, SEEK is 0-based
    if (parsedSpineIndex >= 0 && parsedSpineIndex < epub->getSpineItemsCount()) {
      result.spineIndex = parsedSpineIndex;
      spineFound = true;
      LOG_DBG("ProgressMapper", "Extracted exact spine index %d from xpath", parsedSpineIndex);
    }
  }

  // Fallback if XPath parsing completely fails
  if (!spineFound) {
    const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);
    const int spineCount = epub->getSpineItemsCount();
    for (int i = 0; i < spineCount; i++) {
      const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
      if (cumulativeSize >= targetBytes) {
        result.spineIndex = i;
        spineFound = true;
        break;
      }
    }
    if (!spineFound && spineCount > 0) {
      result.spineIndex = spineCount - 1;
    }
  }

  // 2. Estimate page number using XPath DOM node heuristic
  if (result.spineIndex < epub->getSpineItemsCount()) {
    const size_t prevCumSize = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
    const size_t currentCumSize = epub->getCumulativeSpineItemSize(result.spineIndex);
    const size_t spineSize = currentCumSize - prevCumSize;

    int estimatedTotalPages = 0;
    if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
      estimatedTotalPages = totalPagesInCurrentSpine;
    } else if (currentSpineIndex >= 0 && currentSpineIndex < epub->getSpineItemsCount() &&
               totalPagesInCurrentSpine > 0) {
      const size_t prevCurrCumSize =
          (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const size_t currCumSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
      const size_t currSpineSize = currCumSize - prevCurrCumSize;

      if (currSpineSize > 0) {
        float ratio = static_cast<float>(spineSize) / static_cast<float>(currSpineSize);
        estimatedTotalPages = static_cast<int>(totalPagesInCurrentSpine * ratio);
        if (estimatedTotalPages < 1) estimatedTotalPages = 1;
      }
    }

    result.totalPages = estimatedTotalPages;

    if (estimatedTotalPages > 0) {
      // HEURISTIC: Avoid using global percentage for intra-chapter progress because
      // byte-counting differs wildly between KOReader and SEEK.
      // Instead, extract the last DOM node index from the XPath (e.g., the '5' in '/p[5]').
      int lastNodeIndex = 1;
      size_t lastBracket = koPos.xpath.find_last_of('[');
      size_t docFragBracket = koPos.xpath.find("DocFragment[");

      // Ensure we are parsing a bracket that comes AFTER the chapter declaration
      if (lastBracket != std::string::npos && docFragBracket != std::string::npos &&
          lastBracket > docFragBracket + 15) {
        sscanf(koPos.xpath.c_str() + lastBracket, "[%d]", &lastNodeIndex);
      } else {
        lastNodeIndex = 1;  // Top of the chapter
      }

      // Assume roughly 6 DOM text nodes (paragraphs/divs) fit on this specific E-ink screen.
      int estimatedPage = (lastNodeIndex <= 1) ? 0 : (lastNodeIndex - 1) / 6;

      result.pageNumber = std::max(0, std::min(estimatedPage, estimatedTotalPages - 1));
    }
  }

  LOG_DBG("ProgressMapper", "KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d", koPos.percentage * 100,
          koPos.xpath.c_str(), result.spineIndex, result.pageNumber);

  return result;
}

std::string ProgressMapper::generateXPath(int spineIndex, int pageNumber, int totalPages) {
  // Use 1-based DocFragment indices for KOReader to prevent off-by-one chapter sync errors.
  std::string basePath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";

  if (totalPages > 1 && pageNumber > 0) {
    // HEURISTIC: Create a proportional fake node index. Assume ~6 nodes per page.
    int fakeNodeIndex = (pageNumber * 6) + 1;
    return basePath + "/p[" + std::to_string(fakeNodeIndex) + "]";
  }

  return basePath;
}
