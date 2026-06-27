#ifndef IMAGE_H
#define IMAGE_H

#include "common.h"

// Image data structure
typedef struct {
    unsigned char *data;
    int width;
    int height;
    int channels; // 1 = grayscale, 3 = RGB, 4 = RGBA
    ImageFormat format;
    unsigned char *icc_profile;
    size_t icc_size;
    BOOL pooled;
} ImageData;

// Load image from file
ImageData* image_load(const wchar_t *path);
ImageData* image_load_ex(const wchar_t *path, BOOL prefer_rgb);

#ifdef HAVE_VIPS
BOOL image_init_vips(void);
void image_shutdown_vips(void);
BOOL image_convert_jpeg_to_webp_vips(const wchar_t *path, const wchar_t *out_path,
                                     int quality, int effort, int max_resolution, int resolution_percent);
#endif

#ifdef HAVE_JPEG
// Load JPEG from memory buffer
ImageData* image_load_jpeg_from_memory(const unsigned char *data, size_t size, BOOL prefer_rgb);

#ifdef HAVE_TURBOJPEG
// JPEG YUV fast-paths (no-resize only)
#define YUV_FAST_STAGE_NONE   0
#define YUV_FAST_STAGE_DECODE 1
#define YUV_FAST_STAGE_ENCODE 2

BOOL image_convert_jpeg_to_jpeg_yuv_fast(const wchar_t *path, const wchar_t *out_path, int quality,
                                         const unsigned char *data, size_t size, int *out_stage);
#ifdef HAVE_WEBP
BOOL image_convert_jpeg_to_webp_yuv_fast(const wchar_t *path, const wchar_t *out_path, int quality,
                                         const unsigned char *data, size_t size, int *out_stage);
#endif
// Read JPEG subsampling/colorspace from file
BOOL image_get_jpeg_info(const wchar_t *path, int *out_subsamp, int *out_colorspace);
BOOL image_get_jpeg_info_from_memory(const unsigned char *data, size_t size, int *out_subsamp, int *out_colorspace);
#endif
#endif

// Free image data
void image_free(ImageData *img);
void image_release(ImageData *img);

// Resize image
ImageData* image_resize(ImageData *src, int new_width, int new_height);

// Save image to file
BOOL image_save(ImageData *img, const wchar_t *path, ImageFormat format, int quality);

// Get quality for optimized mode (subjectively <1% visual loss)
int get_optimized_quality(ImageFormat format, int original_quality);

// SIMD level for resize path (1=SSE2, 2=AVX, 3=AVX2)
int image_get_simd_level(void);

#endif // IMAGE_H
