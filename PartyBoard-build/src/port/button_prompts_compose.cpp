// Windows side of button_prompts_compose.m: the same two functions, giving the
// same pictures (see button_prompts.cpp).
//
// The Mac composer scales each label with Core Graphics' high-quality image
// filter, which is Apple's own; no other library reproduces its pixels
// exactly. So that Windows shows exactly what the Mac shows, a Mac draws every
// picture these functions can be asked for once, with button_prompts_compose.m
// itself (tools/button-prompts/precompose.cpp), into
//     ButtonPrompts/<family>/composed/<texture>/
// and this file only puts those pictures together:
//  - a 3D button texture is the Mac's picture for that label, copied;
//  - a strip is stacked from cells the Mac drew one at a time (<glyph>.png,
//    and <glyph>_dim.png for the darkened copies). Each cell is clipped to
//    itself, so a strip the Mac draws in one go has the same pixels.
// params.txt in each folder records the box or cell size the pictures were
// drawn for. If a target changes without precompose being run again, the
// texture keeps the GameCube original and game.log says why, rather than a
// wrong picture being shown.
//
// Paths arrive as UTF-8.

#include <png.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path from_utf8(const char *text)
{
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(text)));
}

std::string to_utf8(const fs::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

bool read_file(const fs::path &path, std::vector<unsigned char> &bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

struct PngInput {
    const std::vector<unsigned char> *bytes;
    size_t offset;
};

void read_png_bytes(png_structp png, png_bytep out, size_t length)
{
    auto *input = static_cast<PngInput *>(png_get_io_ptr(png));
    if (length > input->bytes->size() - input->offset) {
        png_error(png, "file is truncated");
    }
    std::memcpy(out, input->bytes->data() + input->offset, length);
    input->offset += length;
}

void write_png_bytes(png_structp png, png_bytep data, size_t length)
{
    auto *output = static_cast<std::vector<unsigned char> *>(png_get_io_ptr(png));
    output->insert(output->end(), data, data + length);
}

void flush_png_bytes(png_structp) { }

// Decodes the way Aurora's png_io.cpp decodes a replacement texture (8-bit
// RGBA, colour chunks ignored), so the pixels compared are the game's.
bool load_rgba(const fs::path &path, std::vector<unsigned char> &rgba, unsigned &width, unsigned &height)
{
    std::vector<unsigned char> bytes;
    if (!read_file(path, bytes)) {
        return false;
    }
    PngInput input { &bytes, 0 };
    std::vector<png_bytep> rows;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png != nullptr ? png_create_info_struct(png) : nullptr;
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }
    png_set_read_fn(png, &input, read_png_bytes);
    png_read_info(png, info);
    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_expand(png);
    png_set_strip_16(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != size_t(width) * 4) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }
    rgba.assign(size_t(width) * height * 4, 0);
    rows.resize(height);
    for (unsigned y = 0; y < height; ++y) {
        rows[y] = rgba.data() + size_t(y) * width * 4;
    }
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    return true;
}

bool save_rgba(const fs::path &path, const std::vector<unsigned char> &rgba, unsigned width, unsigned height)
{
    std::vector<unsigned char> bytes;
    std::vector<png_bytep> rows(height);
    for (unsigned y = 0; y < height; ++y) {
        rows[y] = const_cast<png_bytep>(rgba.data() + size_t(y) * width * 4);
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png != nullptr ? png_create_info_struct(png) : nullptr;
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_set_write_fn(png, &bytes, write_png_bytes, flush_png_bytes);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    return file.good();
}

// The pictures in <folder> were drawn for these parameters (precompose.cpp
// writes the same line).
bool drawn_for(const fs::path &folder, const char *params)
{
    std::vector<unsigned char> bytes;
    if (!read_file(folder / "params.txt", bytes)) {
        std::fprintf(stderr, "[prompts] missing %s\n", to_utf8(folder / "params.txt").c_str());
        return false;
    }
    std::string drawn(bytes.begin(), bytes.end());
    while (!drawn.empty() && (drawn.back() == '\n' || drawn.back() == '\r')) {
        drawn.pop_back();
    }
    if (drawn != params) {
        std::fprintf(stderr, "[prompts] %s was drawn for \"%s\", the game needs \"%s\" (run precompose again)\n",
            to_utf8(folder).c_str(), drawn.c_str(), params);
        return false;
    }
    return true;
}

} // namespace

// basePath: ButtonPrompts/3d/<texture>.png, labelPath:
// ButtonPrompts/<family>/label/<file>; the Mac's picture of that label on that
// texture is ButtonPrompts/<family>/composed/<texture>/<file>.
extern "C" int PartyBoard_ComposeButtonTexture(const char *basePath, const char *labelPath, const char *outPath,
    int origWidth, float x0, float y0, float x1, float y1)
{
    const fs::path base = from_utf8(basePath), label = from_utf8(labelPath), out = from_utf8(outPath);
    std::error_code ec;
    // The Mac composer fails when either picture is missing; so does this.
    if (origWidth <= 0 || !fs::is_regular_file(base, ec) || !fs::is_regular_file(label, ec)) {
        return 0;
    }
    const fs::path folder = label.parent_path().parent_path() / "composed" / base.stem();
    char params[128];
    std::snprintf(params, sizeof(params), "texture %d %g %g %g %g", origWidth, x0, y0, x1, y1);
    if (!drawn_for(folder, params)) {
        return 0;
    }
    fs::copy_file(folder / label.filename(), out, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "[prompts] cannot copy %s: %s\n", to_utf8(folder / label.filename()).c_str(),
            ec.message().c_str());
        return 0;
    }
    return 1;
}

// glyphPaths[i]: ButtonPrompts/<family>/<glyph>.png or null; the Mac's cell for
// it is ButtonPrompts/<family>/composed/<strip texture>/<glyph>[_dim].png.
extern "C" int PartyBoard_ComposeButtonStrip(const char *outPath, int cellW, int cellH, int k,
    const char *const *glyphPaths, int count, int dimFrom, float dim)
{
    if (cellW <= 0 || cellH <= 0 || k <= 0 || count <= 0) {
        return 0;
    }
    const fs::path out = from_utf8(outPath);
    const unsigned width = unsigned(cellW * k), cellHeight = unsigned(cellH * k);
    const size_t cellBytes = size_t(width) * cellHeight * 4;
    char params[128];
    std::snprintf(params, sizeof(params), "strip %d %d %d %g", cellW, cellH, k, dim);
    // Clear, like the Mac's canvas; a cell without a glyph stays clear.
    std::vector<unsigned char> rgba(cellBytes * count, 0);
    for (int i = 0; i < count; ++i) {
        std::error_code ec;
        if (glyphPaths[i] == nullptr) {
            continue;
        }
        const fs::path glyph = from_utf8(glyphPaths[i]);
        if (!fs::is_regular_file(glyph, ec)) {
            continue;
        }
        const fs::path folder = glyph.parent_path() / "composed" / out.stem();
        if (!drawn_for(folder, params)) {
            return 0;
        }
        fs::path cell = folder / glyph.stem();
        cell += i >= dimFrom ? "_dim.png" : ".png";
        std::vector<unsigned char> pixels;
        unsigned w = 0, h = 0;
        if (!load_rgba(cell, pixels, w, h) || w != width || h != cellHeight) {
            std::fprintf(stderr, "[prompts] cannot use %s\n", to_utf8(cell).c_str());
            return 0;
        }
        std::memcpy(rgba.data() + cellBytes * i, pixels.data(), cellBytes);
    }
    return save_rgba(out, rgba, width, cellHeight * unsigned(count)) ? 1 : 0;
}
