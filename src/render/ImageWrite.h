#pragma once
// PNG output (screenshots, thumbnails, render tests).
#include <filesystem>

namespace rift {
// `rgba`: width * height * 4 bytes, top row first.
bool writePng(const std::filesystem::path& file, int width, int height, const unsigned char* rgba);
}
