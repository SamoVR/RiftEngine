#pragma once
//
// BrickColor.h -- Roblox's named colours (BrickColor.new("Bright red")), for
// scripts and for the editor's colour palette.
//
#include "scene/Value.h"

#include <string>
#include <vector>

namespace rift {

struct BrickColorEntry {
    int number;          // Roblox's BrickColor number
    const char* name;
    Color3 color;
};

// The palette, in the order the editor shows it (light to dark, by hue).
const std::vector<BrickColorEntry>& brickColors();
// By name (exact), -1 if unknown.
int brickColorByName(const std::string& name);
// By Roblox number, -1 if unknown.
int brickColorByNumber(int number);
// The closest named colour to `c`.
int nearestBrickColor(const Color3& c);

} // namespace rift
