# ── Auto-embedded images (assets/*.{png,gif}) ─────────────────────────
# PNG and GIF files placed in the project-root assets/ folder are
# embedded directly into the firmware flash via EMBED_FILES.
#
# At runtime:
#   *.png → decoded by the built-in PNG decoder (png_decode_rgba),
#           scaled to fit the panel with bilinear resampling, alpha-
#           blended over white, dithered to RGB565.
#   *.gif → played by GifAnimator (FreeRTOS task, per-frame decode).
#
# `file(GLOB)` is intentionally without `CONFIGURE_DEPENDS` because
# ESP-IDF includes this CMakeLists in script mode for component
# requirements collection, where CONFIGURE_DEPENDS is invalid.
# tools/import-images.ps1 bumps this file's mtime to force a reconfigure
# after copying new assets.

set(ASSET_DIR "${CMAKE_SOURCE_DIR}/assets")
file(GLOB ASSET_PNGS "${ASSET_DIR}/*.png")
file(GLOB ASSET_GIFS "${ASSET_DIR}/*.gif")
list(SORT ASSET_PNGS)
list(SORT ASSET_GIFS)

# Generate the image lookup table: extern declarations of the embed
# symbols plus embed::display::findImage().
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
set(GEN_CPP "${GEN_DIR}/embedded_images.cpp")
set(GEN_CONTENT "#include \"embedded_images.h\"\n\n#include <cstring>\n\n")
set(TABLE "")
# .png entries — embedded raw, decoded at runtime by drawPngScaled().
foreach(png ${ASSET_PNGS})
    get_filename_component(fname "${png}" NAME)      # barmen-idle.png
    string(MAKE_C_IDENTIFIER "${fname}" sym)         # barmen_idle_png
    string(APPEND GEN_CONTENT
        "extern \"C\" const uint8_t _binary_${sym}_start[];\n"
        "extern \"C\" const uint8_t _binary_${sym}_end[];\n")
    string(APPEND TABLE
        "    {\"${fname}\", _binary_${sym}_start,\n"
        "     static_cast<size_t>(_binary_${sym}_end - _binary_${sym}_start)},\n")
endforeach()
# .gif entries — embedded raw, played at runtime via GifAnimator.
foreach(gif ${ASSET_GIFS})
    get_filename_component(fname "${gif}" NAME)      # barmen-idle.gif
    string(MAKE_C_IDENTIFIER "${fname}" sym)         # barmen_idle_gif
    string(APPEND GEN_CONTENT
        "extern \"C\" const uint8_t _binary_${sym}_start[];\n"
        "extern \"C\" const uint8_t _binary_${sym}_end[];\n")
    string(APPEND TABLE
        "    {\"${fname}\", _binary_${sym}_start,\n"
        "     static_cast<size_t>(_binary_${sym}_end - _binary_${sym}_start)},\n")
endforeach()
string(APPEND GEN_CONTENT
    "\nnamespace embed::display {\n\nnamespace {\n\nconst ImageAsset kImages[] = {\n"
    "${TABLE}"
    "    {nullptr, nullptr, 0}, // sentinel\n"
    "};\n\n} // namespace\n\n"
    "const ImageAsset* findImage(const char* name)\n{\n"
    "    if (!name) return nullptr;\n"
    "    for (const ImageAsset* img = kImages; img->name; ++img) {\n"
    "        if (std::strcmp(img->name, name) == 0) return img;\n"
    "    }\n"
    "    return nullptr;\n"
    "}\n\n} // namespace embed::display\n")
file(WRITE "${GEN_CPP}" "${GEN_CONTENT}")

idf_component_register(
    SRCS
        "src/display_service.cpp"
        "src/gif_animator.cpp"
        "src/decoders/png_decoder.cpp"
        "src/decoders/gif_decoder.cpp"
        "src/decoders/tiny_inflate.cpp"
        "${GEN_CPP}"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        ${ASSET_PNGS}
        ${ASSET_GIFS}
    REQUIRES
        embed
        embed_core
        esp_lcd
    PRIV_REQUIRES
        esp_driver_spi
        esp_driver_gpio
        log
        freertos
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions)

# Allow "decoders/..." includes from src/*.cpp and let the generated
# embedded_images.cpp find the hand-written embedded_images.h
target_include_directories(${COMPONENT_LIB} PRIVATE src)
