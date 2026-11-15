#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include "emu_internal.h"

bool emu_write_png(const char *path, const uint16_t *pixels, int width, int height)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    uint8_t *row = malloc((size_t)width * 3);
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (row == NULL || info == NULL || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(row);
        fclose(f);
        remove(path);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < height; y++) {
        const uint16_t *src = pixels + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            uint16_t c = src[x];
            row[x * 3] = ((c >> 11) & 31) * 255 / 31;
            row[x * 3 + 1] = ((c >> 5) & 63) * 255 / 63;
            row[x * 3 + 2] = (c & 31) * 255 / 31;
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(row);
    return fclose(f) == 0;
}

bool emu_write_png_rgb(const char *path, const void *rgb, int width, int height, int pitch)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (info == NULL || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        remove(path);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < height; y++) {
        png_write_row(png, (png_const_bytep)rgb + (size_t)y * pitch);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    return fclose(f) == 0;
}
