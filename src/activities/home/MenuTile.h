#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct MenuTile {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    std::string label;
    uint32_t iconId; // or std::string iconPath
    std::function<void()> onSelect;
    bool isSelected = false;
};