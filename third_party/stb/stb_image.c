// stb_image is compiled once here.
//
// Only formats a Markdown document plausibly embeds; dimensions are capped so
// a malformed or hostile file can't request gigabytes of pixels.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_MAX_DIMENSIONS 16384
#define STBI_NO_STDIO // files are read with SDL_LoadFile
#include "stb_image.h"
