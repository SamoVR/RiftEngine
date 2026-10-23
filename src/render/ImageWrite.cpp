#include "render/ImageWrite.h"
#include "core/Paths.h"

#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace rift {

bool writePng(const std::filesystem::path& file, int width, int height, const unsigned char* rgba) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    return stbi_write_png(Paths::toUtf8(file).c_str(), width, height, 4, rgba, width * 4) != 0;
}

} // namespace rift
