#include "scene/BrickColor.h"

#include <cmath>

namespace rift {

namespace {
    constexpr Color3 rgb(int r, int g, int b) { return { r / 255.0f, g / 255.0f, b / 255.0f }; }
}

const std::vector<BrickColorEntry>& brickColors() {
    // Eight rows of eight: greys, reds and pinks, oranges and browns, yellows,
    // greens, blues, purples, pastels.
    static const std::vector<BrickColorEntry> table = {
        { 1001, "Institutional white", rgb(248, 248, 248) }, { 1, "White", rgb(242, 243, 243) }, { 208, "Light stone grey", rgb(229, 228, 223) },
        { 1002, "Mid gray", rgb(205, 205, 205) }, { 194, "Medium stone grey", rgb(163, 162, 165) }, { 199, "Dark stone grey", rgb(99, 95, 98) },
        { 26, "Black", rgb(27, 42, 53) }, { 1003, "Really black", rgb(17, 17, 17) },

        { 1004, "Really red", rgb(255, 0, 0) }, { 21, "Bright red", rgb(196, 40, 28) }, { 1007, "Dusty Rose", rgb(163, 75, 75) },
        { 101, "Medium red", rgb(218, 134, 122) }, { 1016, "Pink", rgb(255, 102, 204) }, { 1032, "Hot pink", rgb(255, 0, 191) },
        { 330, "Carnation pink", rgb(255, 152, 220) }, { 9, "Light reddish violet", rgb(232, 186, 200) },

        { 1005, "Deep orange", rgb(255, 176, 0) }, { 106, "Bright orange", rgb(218, 133, 65) }, { 105, "Br. yellowish orange", rgb(226, 155, 64) },
        { 125, "Light orange", rgb(234, 184, 146) }, { 18, "Nougat", rgb(204, 142, 105) }, { 38, "Dark orange", rgb(160, 95, 53) },
        { 217, "Brown", rgb(124, 92, 70) }, { 192, "Reddish brown", rgb(105, 64, 40) },

        { 1009, "New Yeller", rgb(255, 255, 0) }, { 24, "Bright yellow", rgb(245, 205, 48) }, { 333, "Gold", rgb(239, 184, 56) },
        { 226, "Cool yellow", rgb(253, 234, 141) }, { 5, "Brick yellow", rgb(215, 197, 154) }, { 1008, "Olive", rgb(193, 190, 66) },
        { 1014, "CGA brown", rgb(170, 85, 0) }, { 153, "Sand red", rgb(149, 121, 119) },

        { 1020, "Lime green", rgb(0, 255, 0) }, { 119, "Br. yellowish green", rgb(164, 189, 71) }, { 29, "Medium green", rgb(161, 196, 140) },
        { 37, "Bright green", rgb(75, 151, 75) }, { 1021, "Camo", rgb(58, 125, 21) }, { 28, "Dark green", rgb(40, 127, 71) },
        { 1022, "Grime", rgb(127, 142, 100) }, { 141, "Earth green", rgb(39, 70, 45) },

        { 1019, "Toothpaste", rgb(0, 255, 255) }, { 1018, "Teal", rgb(18, 238, 212) }, { 107, "Bright bluish green", rgb(0, 143, 156) },
        { 1013, "Cyan", rgb(4, 175, 236) }, { 23, "Bright blue", rgb(13, 105, 172) }, { 1010, "Really blue", rgb(0, 0, 255) },
        { 1012, "Deep blue", rgb(33, 84, 185) }, { 1011, "Navy blue", rgb(0, 32, 96) },

        { 1006, "Alder", rgb(180, 128, 255) }, { 1031, "Royal purple", rgb(98, 37, 209) }, { 1023, "Lavender", rgb(140, 91, 159) },
        { 104, "Bright violet", rgb(107, 50, 124) }, { 1015, "Magenta", rgb(170, 0, 170) }, { 316, "Plum", rgb(123, 0, 123) },
        { 312, "Mulberry", rgb(89, 34, 89) }, { 135, "Sand blue", rgb(116, 134, 157) },

        { 1024, "Pastel light blue", rgb(175, 221, 255) }, { 45, "Light blue", rgb(180, 210, 228) }, { 1027, "Pastel blue-green", rgb(159, 243, 233) },
        { 1028, "Pastel green", rgb(204, 255, 204) }, { 1029, "Pastel yellow", rgb(255, 255, 204) }, { 1030, "Pastel brown", rgb(255, 204, 153) },
        { 1025, "Pastel orange", rgb(255, 201, 201) }, { 1026, "Pastel violet", rgb(177, 167, 255) },
    };
    return table;
}

int brickColorByName(const std::string& name) {
    const auto& t = brickColors();
    for (size_t i = 0; i < t.size(); ++i) if (name == t[i].name) return static_cast<int>(i);
    return -1;
}

int brickColorByNumber(int number) {
    const auto& t = brickColors();
    for (size_t i = 0; i < t.size(); ++i) if (t[i].number == number) return static_cast<int>(i);
    return -1;
}

int nearestBrickColor(const Color3& c) {
    const auto& t = brickColors();
    int best = 0;
    float bestD = 1e9f;
    for (size_t i = 0; i < t.size(); ++i) {
        const float dr = t[i].color.r - c.r, dg = t[i].color.g - c.g, db = t[i].color.b - c.b;
        const float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
        if (d < bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}

} // namespace rift
