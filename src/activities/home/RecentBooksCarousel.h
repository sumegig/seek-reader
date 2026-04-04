#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct CarouselBook {
    std::string title;
    std::string author;
    std::string thumbBmpPath;
    std::string bookPath;
};

class RecentBooksCarousel {
public:
    RecentBooksCarousel();
    
    // Load recent books from stats or library metadata
    void loadRecent(uint8_t maxBooks = 6);
    
    // Navigation
    void selectNext();
    void selectPrev();
    void selectAtIndex(uint8_t idx);
    
    // Getters
    uint8_t getSelectedIndex() const { return selectedIndex; }
    uint8_t getVisibleOffset() const { return (selectedIndex / 3) * 3; } // 3 per row
    uint8_t getTotalBooks() const { return books.size(); }
    const CarouselBook& getSelected() const { return books[selectedIndex]; }
    const CarouselBook& getBook(uint8_t idx) const { return books[idx]; }
    
private:
    std::vector<CarouselBook> books;
    uint8_t selectedIndex = 0;
};