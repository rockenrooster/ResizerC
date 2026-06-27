#include "image.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include <immintrin.h>
#include <intrin.h>
#include <setjmp.h>

typedef struct {
    int width_capacity;
    int height_capacity;
    int *x0s;
    int *x1s;
    float *fxs;
    int *y0s;
    int *y1s;
    float *fys;
} ResizeScratch;

static _Thread_local ResizeScratch g_resize_scratch = {0};
static _Thread_local unsigned char *g_jpeg_row_buf = NULL;
static _Thread_local size_t g_jpeg_row_capacity = 0;
static _Thread_local unsigned char *g_jpeg_encode_row_buf = NULL;
static _Thread_local size_t g_jpeg_encode_row_capacity = 0;
static _Thread_local ImageData *g_resize_pool[2] = {NULL, NULL};
static _Thread_local int g_resize_pool_index = 0;
static _Thread_local ImageData *g_decode_pool[2] = {NULL, NULL};
static _Thread_local int g_decode_pool_index = 0;

enum {
    SIMD_SSE2 = 1,
    SIMD_AVX = 2,
    SIMD_AVX2 = 3
};

int image_get_simd_level(void) {
    static LONG init = 0;
    static int level = SIMD_SSE2;
    if (InterlockedCompareExchange(&init, 1, 1) == 1) {
        return level;
    }

    int regs[4] = {0};
    __cpuid(regs, 1);
    int osxsave = (regs[2] >> 27) & 1;
    int avx = (regs[2] >> 28) & 1;
    int avx_supported = 0;

    if (osxsave && avx) {
        unsigned long long xcr0 = _xgetbv(0);
        if ((xcr0 & 0x6) == 0x6) {
            avx_supported = 1;
        }
    }

#if defined(__AVX__)
    if (avx_supported) {
        level = SIMD_AVX;
    }
#endif

#if defined(__AVX2__)
    if (avx_supported) {
        __cpuidex(regs, 7, 0);
        if ((regs[1] & (1 << 5)) != 0) {
            level = SIMD_AVX2;
        }
    }
#endif

    InterlockedExchange(&init, 1);
    return level;
}

static void resize_scratch_ensure(int new_width, int new_height) {
    if (new_width > g_resize_scratch.width_capacity) {
        int cap = new_width;
        g_resize_scratch.x0s = (int*)realloc(g_resize_scratch.x0s, sizeof(int) * (size_t)cap);
        g_resize_scratch.x1s = (int*)realloc(g_resize_scratch.x1s, sizeof(int) * (size_t)cap);
        g_resize_scratch.fxs = (float*)realloc(g_resize_scratch.fxs, sizeof(float) * (size_t)cap);
        g_resize_scratch.width_capacity = cap;
    }
    if (new_height > g_resize_scratch.height_capacity) {
        int cap = new_height;
        g_resize_scratch.y0s = (int*)realloc(g_resize_scratch.y0s, sizeof(int) * (size_t)cap);
        g_resize_scratch.y1s = (int*)realloc(g_resize_scratch.y1s, sizeof(int) * (size_t)cap);
        g_resize_scratch.fys = (float*)realloc(g_resize_scratch.fys, sizeof(float) * (size_t)cap);
        g_resize_scratch.height_capacity = cap;
    }
}

static unsigned char *jpeg_row_buffer_ensure(size_t row_stride) {
    if (row_stride > g_jpeg_row_capacity) {
        unsigned char *next = (unsigned char*)realloc(g_jpeg_row_buf, row_stride);
        if (!next) return NULL;
        g_jpeg_row_buf = next;
        g_jpeg_row_capacity = row_stride;
    }
    return g_jpeg_row_buf;
}

static unsigned char *jpeg_encode_row_buffer_ensure(size_t row_stride) {
    if (row_stride > g_jpeg_encode_row_capacity) {
        unsigned char *next = (unsigned char*)realloc(g_jpeg_encode_row_buf, row_stride);
        if (!next) return NULL;
        g_jpeg_encode_row_buf = next;
        g_jpeg_encode_row_capacity = row_stride;
    }
    return g_jpeg_encode_row_buf;
}

// Forward declarations for static functions
static ImageData* image_alloc(int width, int height, int channels);
static BOOL image_save_bmp(ImageData *img, const wchar_t *path);
static BOOL image_save_ico(ImageData *img, const wchar_t *path);
#ifdef HAVE_JPEG
static BOOL image_save_jpeg(ImageData *img, const wchar_t *path, int quality);
static ImageData* image_load_jpeg(const wchar_t *path, BOOL prefer_rgb);
#ifdef HAVE_TURBOJPEG
static BOOL image_save_jpeg_turbo(ImageData *img, const wchar_t *path, int quality);
static ImageData* image_load_jpeg_turbo(const wchar_t *path);
#endif
#endif
#ifdef HAVE_PNG
static BOOL image_save_png(ImageData *img, const wchar_t *path, int quality);
#endif
#ifdef HAVE_WEBP
static BOOL image_save_webp(ImageData *img, const wchar_t *path, int quality, int effort);

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} WebPWriteBuffer;

static _Thread_local WebPWriteBuffer g_webp_writer = {0};

static int webp_write_buffer(const uint8_t *data, size_t data_size, const WebPPicture *picture) {
    WebPWriteBuffer *buf = (WebPWriteBuffer*)picture->custom_ptr;
    if (!buf || !data || data_size == 0) return 0;
    size_t needed = buf->size + data_size;
    if (needed > buf->capacity) {
        size_t new_cap = buf->capacity ? buf->capacity * 2 : 65536;
        if (new_cap < needed) new_cap = needed;
        uint8_t *next = (uint8_t*)realloc(buf->data, new_cap);
        if (!next) return 0;
        buf->data = next;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, data, data_size);
    buf->size = needed;
    return 1;
}

static void webp_writer_reset(WebPWriteBuffer *buf) {
    if (!buf) return;
    buf->size = 0;
}
#endif

#ifdef HAVE_VIPS
static char* wide_to_utf8_alloc(const wchar_t *wstr) {
    if (!wstr) return NULL;
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) return NULL;
    char *buf = (char*)malloc((size_t)needed);
    if (!buf) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf, needed, NULL, NULL) <= 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static LONG g_vips_initialized = 0;

BOOL image_init_vips(void) {
    if (InterlockedCompareExchange(&g_vips_initialized, 1, 1) == 1) {
        return TRUE;
    }
    if (vips_init("ResizerC") != 0) {
        vips_error_clear();
        return FALSE;
    }
    vips_concurrency_set(1);
    InterlockedExchange(&g_vips_initialized, 1);
    return TRUE;
}

void image_shutdown_vips(void) {
    if (InterlockedCompareExchange(&g_vips_initialized, 0, 1) == 1) {
        vips_shutdown();
    }
}
#endif

static BOOL jpeg_insert_icc_profile(const unsigned char *jpeg_buf, size_t jpeg_size,
                                    const unsigned char *icc, size_t icc_size,
                                    unsigned char **out_buf, size_t *out_size);

static void image_clear_icc(ImageData *img) {
    if (!img) return;
    if (img->icc_profile) {
        free(img->icc_profile);
    }
    img->icc_profile = NULL;
    img->icc_size = 0;
}

static BOOL image_set_icc(ImageData *img, const unsigned char *icc, size_t size) {
    if (!img) return FALSE;
    image_clear_icc(img);
    if (!icc || size == 0) return TRUE;
    img->icc_profile = (unsigned char*)malloc(size);
    if (!img->icc_profile) {
        img->icc_size = 0;
        return FALSE;
    }
    memcpy(img->icc_profile, icc, size);
    img->icc_size = size;
    return TRUE;
}

static BOOL image_copy_icc(ImageData *dst, const ImageData *src) {
    if (!dst) return FALSE;
    if (!src || !src->icc_profile || src->icc_size == 0) {
        image_clear_icc(dst);
        return TRUE;
    }
    return image_set_icc(dst, src->icc_profile, src->icc_size);
}

// Allocate new image
static ImageData* image_alloc(int width, int height, int channels) {
    ImageData *img = (ImageData*)malloc(sizeof(ImageData));
    if (!img) return NULL;

    img->width = width;
    img->height = height;
    img->channels = channels;
    img->data = (unsigned char*)malloc((size_t)width * height * channels);
    img->icc_profile = NULL;
    img->icc_size = 0;
    img->pooled = FALSE;

    if (!img->data) {
        free(img);
        return NULL;
    }

    return img;
}

static ImageData* image_alloc_or_resize(ImageData *img, int width, int height, int channels, BOOL pooled) {
    if (!img) {
        ImageData *created = image_alloc(width, height, channels);
        if (created) {
            created->pooled = pooled;
        }
        return created;
    }
    image_clear_icc(img);
    size_t needed = (size_t)width * height * channels;
    size_t current = (size_t)img->width * img->height * img->channels;
    if (img->channels != channels || current < needed) {
        unsigned char *next = (unsigned char*)realloc(img->data, needed);
        if (!next) return NULL;
        img->data = next;
        img->channels = channels;
    }
    img->width = width;
    img->height = height;
    img->pooled = pooled;
    return img;
}

// Free image data
void image_free(ImageData *img) {
    if (img) {
        image_clear_icc(img);
        if (img->data) free(img->data);
        free(img);
    }
}

void image_release(ImageData *img) {
    if (!img) return;
    if (img->pooled) {
        image_clear_icc(img);
        return;
    }
    image_free(img);
}

static void resize_bilinear_rgba_sse2(unsigned char *dst_row,
                                      const unsigned char *row0,
                                      const unsigned char *row1,
                                      const int *x0s,
                                      const int *x1s,
                                      const float *fxs,
                                      int new_width,
                                      float fy,
                                      float one_minus_fy) {
    __m128 one_minus_fy_v = _mm_set1_ps(one_minus_fy);
    __m128 fy_v = _mm_set1_ps(fy);
    __m128 zero = _mm_setzero_ps();
    __m128i zero_i = _mm_setzero_si128();

    for (int x = 0; x < new_width; x++) {
        int x0 = x0s[x];
        int x1 = x1s[x];
        float fx = fxs[x];
        float one_minus_fx = 1.0f - fx;

        const unsigned char *p00 = &row0[x0 * 4];
        const unsigned char *p10 = &row0[x1 * 4];
        const unsigned char *p01 = &row1[x0 * 4];
        const unsigned char *p11 = &row1[x1 * 4];
        unsigned char *out = &dst_row[x * 4];

        __m128 w00 = _mm_mul_ps(_mm_set1_ps(one_minus_fx), one_minus_fy_v);
        __m128 w10 = _mm_mul_ps(_mm_set1_ps(fx), one_minus_fy_v);
        __m128 w01 = _mm_mul_ps(_mm_set1_ps(one_minus_fx), fy_v);
        __m128 w11 = _mm_mul_ps(_mm_set1_ps(fx), fy_v);

        __m128i p00i = _mm_cvtsi32_si128(*(const int*)p00);
        __m128i p10i = _mm_cvtsi32_si128(*(const int*)p10);
        __m128i p01i = _mm_cvtsi32_si128(*(const int*)p01);
        __m128i p11i = _mm_cvtsi32_si128(*(const int*)p11);

        p00i = _mm_unpacklo_epi8(p00i, zero_i);
        p00i = _mm_unpacklo_epi16(p00i, zero_i);
        p10i = _mm_unpacklo_epi8(p10i, zero_i);
        p10i = _mm_unpacklo_epi16(p10i, zero_i);
        p01i = _mm_unpacklo_epi8(p01i, zero_i);
        p01i = _mm_unpacklo_epi16(p01i, zero_i);
        p11i = _mm_unpacklo_epi8(p11i, zero_i);
        p11i = _mm_unpacklo_epi16(p11i, zero_i);

        __m128 p00f = _mm_cvtepi32_ps(p00i);
        __m128 p10f = _mm_cvtepi32_ps(p10i);
        __m128 p01f = _mm_cvtepi32_ps(p01i);
        __m128 p11f = _mm_cvtepi32_ps(p11i);

        __m128 acc = _mm_add_ps(_mm_add_ps(_mm_mul_ps(p00f, w00), _mm_mul_ps(p10f, w10)),
                                _mm_add_ps(_mm_mul_ps(p01f, w01), _mm_mul_ps(p11f, w11)));
        acc = _mm_max_ps(acc, zero);
        __m128i outi = _mm_cvtps_epi32(acc);
        __m128i pack16 = _mm_packs_epi32(outi, outi);
        __m128i pack8 = _mm_packus_epi16(pack16, pack16);
        *(int*)out = _mm_cvtsi128_si32(pack8);
    }
}

static void resize_bilinear_rgb_sse2(unsigned char *dst_row,
                                     const unsigned char *row0,
                                     const unsigned char *row1,
                                     const int *x0s,
                                     const int *x1s,
                                     const float *fxs,
                                     int new_width,
                                     float fy,
                                     float one_minus_fy) {
    __m128 one_minus_fy_v = _mm_set1_ps(one_minus_fy);
    __m128 fy_v = _mm_set1_ps(fy);
    __m128 zero = _mm_setzero_ps();
    __m128i zero_i = _mm_setzero_si128();

    for (int x = 0; x < new_width; x++) {
        int x0 = x0s[x];
        int x1 = x1s[x];
        float fx = fxs[x];
        float one_minus_fx = 1.0f - fx;

        const unsigned char *p00 = &row0[x0 * 3];
        const unsigned char *p10 = &row0[x1 * 3];
        const unsigned char *p01 = &row1[x0 * 3];
        const unsigned char *p11 = &row1[x1 * 3];
        unsigned char *out = &dst_row[x * 3];

        __m128 w00 = _mm_mul_ps(_mm_set1_ps(one_minus_fx), one_minus_fy_v);
        __m128 w10 = _mm_mul_ps(_mm_set1_ps(fx), one_minus_fy_v);
        __m128 w01 = _mm_mul_ps(_mm_set1_ps(one_minus_fx), fy_v);
        __m128 w11 = _mm_mul_ps(_mm_set1_ps(fx), fy_v);

        unsigned int p00v = (unsigned int)p00[0] | ((unsigned int)p00[1] << 8) | ((unsigned int)p00[2] << 16);
        unsigned int p10v = (unsigned int)p10[0] | ((unsigned int)p10[1] << 8) | ((unsigned int)p10[2] << 16);
        unsigned int p01v = (unsigned int)p01[0] | ((unsigned int)p01[1] << 8) | ((unsigned int)p01[2] << 16);
        unsigned int p11v = (unsigned int)p11[0] | ((unsigned int)p11[1] << 8) | ((unsigned int)p11[2] << 16);

        __m128i p00i = _mm_cvtsi32_si128((int)p00v);
        __m128i p10i = _mm_cvtsi32_si128((int)p10v);
        __m128i p01i = _mm_cvtsi32_si128((int)p01v);
        __m128i p11i = _mm_cvtsi32_si128((int)p11v);

        p00i = _mm_unpacklo_epi8(p00i, zero_i);
        p00i = _mm_unpacklo_epi16(p00i, zero_i);
        p10i = _mm_unpacklo_epi8(p10i, zero_i);
        p10i = _mm_unpacklo_epi16(p10i, zero_i);
        p01i = _mm_unpacklo_epi8(p01i, zero_i);
        p01i = _mm_unpacklo_epi16(p01i, zero_i);
        p11i = _mm_unpacklo_epi8(p11i, zero_i);
        p11i = _mm_unpacklo_epi16(p11i, zero_i);

        __m128 p00f = _mm_cvtepi32_ps(p00i);
        __m128 p10f = _mm_cvtepi32_ps(p10i);
        __m128 p01f = _mm_cvtepi32_ps(p01i);
        __m128 p11f = _mm_cvtepi32_ps(p11i);

        __m128 acc = _mm_add_ps(_mm_add_ps(_mm_mul_ps(p00f, w00), _mm_mul_ps(p10f, w10)),
                                _mm_add_ps(_mm_mul_ps(p01f, w01), _mm_mul_ps(p11f, w11)));
        acc = _mm_max_ps(acc, zero);
        __m128i outi = _mm_cvtps_epi32(acc);
        __m128i pack16 = _mm_packs_epi32(outi, outi);
        __m128i pack8 = _mm_packus_epi16(pack16, pack16);
        unsigned int outv = (unsigned int)_mm_cvtsi128_si32(pack8);

        out[0] = (unsigned char)(outv & 0xFF);
        out[1] = (unsigned char)((outv >> 8) & 0xFF);
        out[2] = (unsigned char)((outv >> 16) & 0xFF);
    }
}

static void resize_bilinear_rgba_avx(unsigned char *dst_row,
                                     const unsigned char *row0,
                                     const unsigned char *row1,
                                     const int *x0s,
                                     const int *x1s,
                                     const float *fxs,
                                     int new_width,
                                     float fy,
                                     float one_minus_fy) {
#if defined(__AVX__)
    __m256 zero = _mm256_setzero_ps();
    __m128i zero_i = _mm_setzero_si128();

    int x = 0;
    for (; x + 1 < new_width; x += 2) {
        int x0a = x0s[x];
        int x1a = x1s[x];
        int x0b = x0s[x + 1];
        int x1b = x1s[x + 1];

        float fxa = fxs[x];
        float one_minus_fxa = 1.0f - fxa;
        float fxb = fxs[x + 1];
        float one_minus_fxb = 1.0f - fxb;

        const unsigned char *p00a = &row0[x0a * 4];
        const unsigned char *p10a = &row0[x1a * 4];
        const unsigned char *p01a = &row1[x0a * 4];
        const unsigned char *p11a = &row1[x1a * 4];
        const unsigned char *p00b = &row0[x0b * 4];
        const unsigned char *p10b = &row0[x1b * 4];
        const unsigned char *p01b = &row1[x0b * 4];
        const unsigned char *p11b = &row1[x1b * 4];

        __m256 w00 = _mm256_set_ps(one_minus_fxb * one_minus_fy, one_minus_fxb * one_minus_fy,
                                   one_minus_fxb * one_minus_fy, one_minus_fxb * one_minus_fy,
                                   one_minus_fxa * one_minus_fy, one_minus_fxa * one_minus_fy,
                                   one_minus_fxa * one_minus_fy, one_minus_fxa * one_minus_fy);
        __m256 w10 = _mm256_set_ps(fxb * one_minus_fy, fxb * one_minus_fy,
                                   fxb * one_minus_fy, fxb * one_minus_fy,
                                   fxa * one_minus_fy, fxa * one_minus_fy,
                                   fxa * one_minus_fy, fxa * one_minus_fy);
        __m256 w01 = _mm256_set_ps(one_minus_fxb * fy, one_minus_fxb * fy,
                                   one_minus_fxb * fy, one_minus_fxb * fy,
                                   one_minus_fxa * fy, one_minus_fxa * fy,
                                   one_minus_fxa * fy, one_minus_fxa * fy);
        __m256 w11 = _mm256_set_ps(fxb * fy, fxb * fy,
                                   fxb * fy, fxb * fy,
                                   fxa * fy, fxa * fy,
                                   fxa * fy, fxa * fy);

        __m128i p00i_a = _mm_cvtsi32_si128(*(const int*)p00a);
        __m128i p10i_a = _mm_cvtsi32_si128(*(const int*)p10a);
        __m128i p01i_a = _mm_cvtsi32_si128(*(const int*)p01a);
        __m128i p11i_a = _mm_cvtsi32_si128(*(const int*)p11a);
        __m128i p00i_b = _mm_cvtsi32_si128(*(const int*)p00b);
        __m128i p10i_b = _mm_cvtsi32_si128(*(const int*)p10b);
        __m128i p01i_b = _mm_cvtsi32_si128(*(const int*)p01b);
        __m128i p11i_b = _mm_cvtsi32_si128(*(const int*)p11b);

        p00i_a = _mm_unpacklo_epi8(p00i_a, zero_i);
        p00i_a = _mm_unpacklo_epi16(p00i_a, zero_i);
        p10i_a = _mm_unpacklo_epi8(p10i_a, zero_i);
        p10i_a = _mm_unpacklo_epi16(p10i_a, zero_i);
        p01i_a = _mm_unpacklo_epi8(p01i_a, zero_i);
        p01i_a = _mm_unpacklo_epi16(p01i_a, zero_i);
        p11i_a = _mm_unpacklo_epi8(p11i_a, zero_i);
        p11i_a = _mm_unpacklo_epi16(p11i_a, zero_i);

        p00i_b = _mm_unpacklo_epi8(p00i_b, zero_i);
        p00i_b = _mm_unpacklo_epi16(p00i_b, zero_i);
        p10i_b = _mm_unpacklo_epi8(p10i_b, zero_i);
        p10i_b = _mm_unpacklo_epi16(p10i_b, zero_i);
        p01i_b = _mm_unpacklo_epi8(p01i_b, zero_i);
        p01i_b = _mm_unpacklo_epi16(p01i_b, zero_i);
        p11i_b = _mm_unpacklo_epi8(p11i_b, zero_i);
        p11i_b = _mm_unpacklo_epi16(p11i_b, zero_i);

        __m256 p00f = _mm256_set_m128(_mm_cvtepi32_ps(p00i_b), _mm_cvtepi32_ps(p00i_a));
        __m256 p10f = _mm256_set_m128(_mm_cvtepi32_ps(p10i_b), _mm_cvtepi32_ps(p10i_a));
        __m256 p01f = _mm256_set_m128(_mm_cvtepi32_ps(p01i_b), _mm_cvtepi32_ps(p01i_a));
        __m256 p11f = _mm256_set_m128(_mm_cvtepi32_ps(p11i_b), _mm_cvtepi32_ps(p11i_a));

        __m256 acc = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(p00f, w00), _mm256_mul_ps(p10f, w10)),
                                   _mm256_add_ps(_mm256_mul_ps(p01f, w01), _mm256_mul_ps(p11f, w11)));
        acc = _mm256_max_ps(acc, zero);
        __m256i outi = _mm256_cvtps_epi32(acc);

        __m128i out_lo = _mm256_extracti128_si256(outi, 0);
        __m128i out_hi = _mm256_extracti128_si256(outi, 1);
        __m128i pack16_lo = _mm_packs_epi32(out_lo, out_lo);
        __m128i pack8_lo = _mm_packus_epi16(pack16_lo, pack16_lo);
        __m128i pack16_hi = _mm_packs_epi32(out_hi, out_hi);
        __m128i pack8_hi = _mm_packus_epi16(pack16_hi, pack16_hi);

        *(int*)&dst_row[x * 4] = _mm_cvtsi128_si32(pack8_lo);
        *(int*)&dst_row[(x + 1) * 4] = _mm_cvtsi128_si32(pack8_hi);
    }

    if (x < new_width) {
        resize_bilinear_rgba_sse2(&dst_row[x * 4], row0, row1, x0s + x, x1s + x, fxs + x, 1, fy, one_minus_fy);
    }
#else
    (void)dst_row;
    (void)row0;
    (void)row1;
    (void)x0s;
    (void)x1s;
    (void)fxs;
    (void)new_width;
    (void)fy;
    (void)one_minus_fy;
#endif
}

static void resize_bilinear_rgb_avx(unsigned char *dst_row,
                                    const unsigned char *row0,
                                    const unsigned char *row1,
                                    const int *x0s,
                                    const int *x1s,
                                    const float *fxs,
                                    int new_width,
                                    float fy,
                                    float one_minus_fy) {
#if defined(__AVX__)
    __m256 zero = _mm256_setzero_ps();
    __m128i zero_i = _mm_setzero_si128();

    int x = 0;
    for (; x + 1 < new_width; x += 2) {
        int x0a = x0s[x];
        int x1a = x1s[x];
        int x0b = x0s[x + 1];
        int x1b = x1s[x + 1];

        float fxa = fxs[x];
        float one_minus_fxa = 1.0f - fxa;
        float fxb = fxs[x + 1];
        float one_minus_fxb = 1.0f - fxb;

        const unsigned char *p00a = &row0[x0a * 3];
        const unsigned char *p10a = &row0[x1a * 3];
        const unsigned char *p01a = &row1[x0a * 3];
        const unsigned char *p11a = &row1[x1a * 3];
        const unsigned char *p00b = &row0[x0b * 3];
        const unsigned char *p10b = &row0[x1b * 3];
        const unsigned char *p01b = &row1[x0b * 3];
        const unsigned char *p11b = &row1[x1b * 3];

        __m256 w00 = _mm256_set_ps(one_minus_fxb * one_minus_fy, one_minus_fxb * one_minus_fy,
                                   one_minus_fxb * one_minus_fy, one_minus_fxb * one_minus_fy,
                                   one_minus_fxa * one_minus_fy, one_minus_fxa * one_minus_fy,
                                   one_minus_fxa * one_minus_fy, one_minus_fxa * one_minus_fy);
        __m256 w10 = _mm256_set_ps(fxb * one_minus_fy, fxb * one_minus_fy,
                                   fxb * one_minus_fy, fxb * one_minus_fy,
                                   fxa * one_minus_fy, fxa * one_minus_fy,
                                   fxa * one_minus_fy, fxa * one_minus_fy);
        __m256 w01 = _mm256_set_ps(one_minus_fxb * fy, one_minus_fxb * fy,
                                   one_minus_fxb * fy, one_minus_fxb * fy,
                                   one_minus_fxa * fy, one_minus_fxa * fy,
                                   one_minus_fxa * fy, one_minus_fxa * fy);
        __m256 w11 = _mm256_set_ps(fxb * fy, fxb * fy,
                                   fxb * fy, fxb * fy,
                                   fxa * fy, fxa * fy,
                                   fxa * fy, fxa * fy);

        unsigned int p00va = (unsigned int)p00a[0] | ((unsigned int)p00a[1] << 8) | ((unsigned int)p00a[2] << 16);
        unsigned int p10va = (unsigned int)p10a[0] | ((unsigned int)p10a[1] << 8) | ((unsigned int)p10a[2] << 16);
        unsigned int p01va = (unsigned int)p01a[0] | ((unsigned int)p01a[1] << 8) | ((unsigned int)p01a[2] << 16);
        unsigned int p11va = (unsigned int)p11a[0] | ((unsigned int)p11a[1] << 8) | ((unsigned int)p11a[2] << 16);
        unsigned int p00vb = (unsigned int)p00b[0] | ((unsigned int)p00b[1] << 8) | ((unsigned int)p00b[2] << 16);
        unsigned int p10vb = (unsigned int)p10b[0] | ((unsigned int)p10b[1] << 8) | ((unsigned int)p10b[2] << 16);
        unsigned int p01vb = (unsigned int)p01b[0] | ((unsigned int)p01b[1] << 8) | ((unsigned int)p01b[2] << 16);
        unsigned int p11vb = (unsigned int)p11b[0] | ((unsigned int)p11b[1] << 8) | ((unsigned int)p11b[2] << 16);

        __m128i p00i_a = _mm_cvtsi32_si128((int)p00va);
        __m128i p10i_a = _mm_cvtsi32_si128((int)p10va);
        __m128i p01i_a = _mm_cvtsi32_si128((int)p01va);
        __m128i p11i_a = _mm_cvtsi32_si128((int)p11va);
        __m128i p00i_b = _mm_cvtsi32_si128((int)p00vb);
        __m128i p10i_b = _mm_cvtsi32_si128((int)p10vb);
        __m128i p01i_b = _mm_cvtsi32_si128((int)p01vb);
        __m128i p11i_b = _mm_cvtsi32_si128((int)p11vb);

        p00i_a = _mm_unpacklo_epi8(p00i_a, zero_i);
        p00i_a = _mm_unpacklo_epi16(p00i_a, zero_i);
        p10i_a = _mm_unpacklo_epi8(p10i_a, zero_i);
        p10i_a = _mm_unpacklo_epi16(p10i_a, zero_i);
        p01i_a = _mm_unpacklo_epi8(p01i_a, zero_i);
        p01i_a = _mm_unpacklo_epi16(p01i_a, zero_i);
        p11i_a = _mm_unpacklo_epi8(p11i_a, zero_i);
        p11i_a = _mm_unpacklo_epi16(p11i_a, zero_i);

        p00i_b = _mm_unpacklo_epi8(p00i_b, zero_i);
        p00i_b = _mm_unpacklo_epi16(p00i_b, zero_i);
        p10i_b = _mm_unpacklo_epi8(p10i_b, zero_i);
        p10i_b = _mm_unpacklo_epi16(p10i_b, zero_i);
        p01i_b = _mm_unpacklo_epi8(p01i_b, zero_i);
        p01i_b = _mm_unpacklo_epi16(p01i_b, zero_i);
        p11i_b = _mm_unpacklo_epi8(p11i_b, zero_i);
        p11i_b = _mm_unpacklo_epi16(p11i_b, zero_i);

        __m256 p00f = _mm256_set_m128(_mm_cvtepi32_ps(p00i_b), _mm_cvtepi32_ps(p00i_a));
        __m256 p10f = _mm256_set_m128(_mm_cvtepi32_ps(p10i_b), _mm_cvtepi32_ps(p10i_a));
        __m256 p01f = _mm256_set_m128(_mm_cvtepi32_ps(p01i_b), _mm_cvtepi32_ps(p01i_a));
        __m256 p11f = _mm256_set_m128(_mm_cvtepi32_ps(p11i_b), _mm_cvtepi32_ps(p11i_a));

        __m256 acc = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(p00f, w00), _mm256_mul_ps(p10f, w10)),
                                   _mm256_add_ps(_mm256_mul_ps(p01f, w01), _mm256_mul_ps(p11f, w11)));
        acc = _mm256_max_ps(acc, zero);
        __m256i outi = _mm256_cvtps_epi32(acc);

        __m128i out_lo = _mm256_extracti128_si256(outi, 0);
        __m128i out_hi = _mm256_extracti128_si256(outi, 1);
        __m128i pack16_lo = _mm_packs_epi32(out_lo, out_lo);
        __m128i pack8_lo = _mm_packus_epi16(pack16_lo, pack16_lo);
        __m128i pack16_hi = _mm_packs_epi32(out_hi, out_hi);
        __m128i pack8_hi = _mm_packus_epi16(pack16_hi, pack16_hi);

        unsigned int outa = (unsigned int)_mm_cvtsi128_si32(pack8_lo);
        unsigned int outb = (unsigned int)_mm_cvtsi128_si32(pack8_hi);

        unsigned char *out0 = &dst_row[x * 3];
        unsigned char *out1 = &dst_row[(x + 1) * 3];
        out0[0] = (unsigned char)(outa & 0xFF);
        out0[1] = (unsigned char)((outa >> 8) & 0xFF);
        out0[2] = (unsigned char)((outa >> 16) & 0xFF);
        out1[0] = (unsigned char)(outb & 0xFF);
        out1[1] = (unsigned char)((outb >> 8) & 0xFF);
        out1[2] = (unsigned char)((outb >> 16) & 0xFF);
    }

    if (x < new_width) {
        resize_bilinear_rgb_sse2(&dst_row[x * 3], row0, row1, x0s + x, x1s + x, fxs + x, 1, fy, one_minus_fy);
    }
#else
    (void)dst_row;
    (void)row0;
    (void)row1;
    (void)x0s;
    (void)x1s;
    (void)fxs;
    (void)new_width;
    (void)fy;
    (void)one_minus_fy;
#endif
}

static void resize_bilinear_rgb_avx2(unsigned char *dst_row,
                                     const unsigned char *row0,
                                     const unsigned char *row1,
                                     const int *x0s,
                                     const int *x1s,
                                     const float *fxs,
                                     int new_width,
                                     float fy,
                                     float one_minus_fy) {
#if defined(__AVX2__)
    resize_bilinear_rgb_avx(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
#else
    resize_bilinear_rgb_sse2(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
#endif
}

static void resize_bilinear_rgba_avx2(unsigned char *dst_row,
                                      const unsigned char *row0,
                                      const unsigned char *row1,
                                      const int *x0s,
                                      const int *x1s,
                                      const float *fxs,
                                      int new_width,
                                      float fy,
                                      float one_minus_fy) {
#if defined(__AVX2__)
    resize_bilinear_rgba_avx(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
#else
    resize_bilinear_rgba_sse2(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
#endif
}

ImageData* image_resize(ImageData *src, int new_width, int new_height) {
    if (!src || !src->data) return NULL;

    int pool_index = g_resize_pool_index;
    g_resize_pool_index = (g_resize_pool_index + 1) & 1;
    ImageData *dst = image_alloc_or_resize(g_resize_pool[pool_index], new_width, new_height, src->channels, TRUE);
    g_resize_pool[pool_index] = dst;
    if (!dst) return NULL;

    float x_ratio = (float)src->width / (float)new_width;
    float y_ratio = (float)src->height / (float)new_height;

    resize_scratch_ensure(new_width, new_height);
    int *x0s = g_resize_scratch.x0s;
    int *x1s = g_resize_scratch.x1s;
    float *fxs = g_resize_scratch.fxs;
    int *y0s = g_resize_scratch.y0s;
    int *y1s = g_resize_scratch.y1s;
    float *fys = g_resize_scratch.fys;

    if (!x0s || !x1s || !fxs || !y0s || !y1s || !fys) {
        image_release(dst);
        return NULL;
    }

    for (int x = 0; x < new_width; x++) {
        float src_x = x * x_ratio;
        int x0 = (int)src_x;
        int x1 = x0 + 1;
        if (x0 < 0) x0 = 0;
        if (x1 >= src->width) x1 = src->width - 1;
        x0s[x] = x0;
        x1s[x] = x1;
        fxs[x] = src_x - x0;
    }

    for (int y = 0; y < new_height; y++) {
        float src_y = y * y_ratio;
        int y0 = (int)src_y;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 >= src->height) y1 = src->height - 1;
        y0s[y] = y0;
        y1s[y] = y1;
        fys[y] = src_y - y0;
    }

    int channels = src->channels;
    for (int y = 0; y < new_height; y++) {
        int y0 = y0s[y];
        int y1 = y1s[y];
        float fy = fys[y];
        float one_minus_fy = 1.0f - fy;

        const unsigned char *row0 = &src->data[(size_t)y0 * src->width * channels];
        const unsigned char *row1 = &src->data[(size_t)y1 * src->width * channels];
        unsigned char *dst_row = &dst->data[(size_t)y * new_width * channels];

        if (channels == 4) {
            int simd = image_get_simd_level();
            if (simd == SIMD_AVX2) {
                resize_bilinear_rgba_avx2(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
            } else if (simd == SIMD_AVX) {
                resize_bilinear_rgba_avx(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
            } else {
                resize_bilinear_rgba_sse2(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
            }
            continue;
        }
        if (channels == 3) {
            int simd = image_get_simd_level();
            if (simd == SIMD_AVX2) {
                resize_bilinear_rgb_avx2(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
            } else if (simd == SIMD_AVX) {
                resize_bilinear_rgb_avx(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
            } else {
                resize_bilinear_rgb_sse2(dst_row, row0, row1, x0s, x1s, fxs, new_width, fy, one_minus_fy);
            }
            continue;
        }

        for (int x = 0; x < new_width; x++) {
            int x0 = x0s[x];
            int x1 = x1s[x];
            float fx = fxs[x];
            float one_minus_fx = 1.0f - fx;

            const unsigned char *p00 = &row0[x0 * channels];
            const unsigned char *p10 = &row0[x1 * channels];
            const unsigned char *p01 = &row1[x0 * channels];
            const unsigned char *p11 = &row1[x1 * channels];
            unsigned char *out = &dst_row[x * channels];

            for (int c = 0; c < channels; c++) {
                float v0 = p00[c];
                float v1 = p10[c];
                float v2 = p01[c];
                float v3 = p11[c];
                float v = v0 * one_minus_fx * one_minus_fy +
                          v1 * fx * one_minus_fy +
                          v2 * one_minus_fx * fy +
                          v3 * fx * fy;
                out[c] = (unsigned char)(v + 0.5f);
            }
        }
    }

    image_copy_icc(dst, src);
    return dst;
}

#ifdef HAVE_JPEG
#ifdef HAVE_TURBOJPEG
static _Thread_local tjhandle g_tj_compress_handle = NULL;
static _Thread_local tjhandle g_tj_decompress_handle = NULL;
static LONG g_logged_tj_yuv_error = 0;
static LONG g_logged_tj_info_error = 0;
static LONG g_logged_jpeg_info_error = 0;
static LONG g_logged_webp_yuv_error = 0;

static tjhandle get_tj_compress_handle(void) {
    if (!g_tj_compress_handle) {
        g_tj_compress_handle = tjInitCompress();
    }
    return g_tj_compress_handle;
}

static tjhandle get_tj_decompress_handle(void) {
    if (!g_tj_decompress_handle) {
        g_tj_decompress_handle = tjInitDecompress();
    }
    return g_tj_decompress_handle;
}

static void log_tj_yuv_error_once(const wchar_t *context, tjhandle tj) {
    if (InterlockedCompareExchange(&g_logged_tj_yuv_error, 1, 0) != 0) return;
    const char *err = tjGetErrorStr2(tj);
    log_error(L"    %s failed: %S", context, err ? err : "unknown");
}

static void log_tj_info_error_once(const wchar_t *context, tjhandle tj) {
    if (InterlockedCompareExchange(&g_logged_tj_info_error, 1, 0) != 0) return;
    const char *err = tjGetErrorStr2(tj);
    log_error(L"    %s failed: %S", context, err ? err : "unknown");
}

static void log_jpeg_info_error_once(const wchar_t *msg, DWORD err) {
    if (InterlockedCompareExchange(&g_logged_jpeg_info_error, 1, 0) != 0) return;
    if (err != 0) {
        log_error(L"    image_get_jpeg_info: %s (error: %lu)", msg, err);
    } else {
        log_error(L"    image_get_jpeg_info: %s", msg);
    }
}

// Check if buffer has valid JPEG signature (magic number 0xFF 0xD8)
static BOOL is_valid_jpeg_signature(const unsigned char *data, size_t size) {
    if (!data || size < 2) return FALSE;
    return (data[0] == 0xFF && data[1] == 0xD8);
}

static void log_webp_yuv_error_once(WebPEncodingError code) {
    if (InterlockedCompareExchange(&g_logged_webp_yuv_error, 1, 0) != 0) return;
    log_error(L"    WebPEncode (YUV) failed: error code %d", (int)code);
}

typedef struct {
    int width;
    int height;
    int y_stride;
    int uv_stride;
    size_t y_size;
    size_t uv_size;
    unsigned char *buffer;
    unsigned char *planes[3];
} JpegYuv420;

typedef struct {
    int width;
    int height;
    int subsamp;
    int align;
    size_t size;
    unsigned char *buffer;
} JpegYuvAny;

// Custom error manager for libjpeg to handle crashes gracefully
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jmp_buffer;
} MyErrorManager;

static void my_jpeg_error_exit(j_common_ptr cinfo) {
    MyErrorManager *myerr = (MyErrorManager*)cinfo->err;
    longjmp(myerr->jmp_buffer, 1);
}

#ifdef HAVE_JPEG
#define ICC_MARKER (JPEG_APP0 + 2)
#define ICC_PROFILE_ID "ICC_PROFILE"
#define ICC_PROFILE_ID_LEN 12
#define ICC_OVERHEAD_LEN 14
#define ICC_MAX_BYTES_IN_MARKER (65533 - ICC_OVERHEAD_LEN)

static BOOL jpeg_extract_icc_from_memory(const unsigned char *data, size_t size,
                                         unsigned char **out_icc, size_t *out_size) {
    if (out_icc) *out_icc = NULL;
    if (out_size) *out_size = 0;
    if (!data || size == 0) return FALSE;

    // Validate JPEG signature before attempting ICC extraction
    // This prevents crashes on corrupted files with invalid magic numbers
    if (!is_valid_jpeg_signature(data, size)) {
        return FALSE;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    BOOL ok = FALSE;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char*)data, (unsigned long)size);
    jpeg_save_markers(&cinfo, ICC_MARKER, 0xFFFF);

    if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
        unsigned char *icc_data = NULL;
        unsigned int icc_len = 0;
        if (jpeg_read_icc_profile(&cinfo, &icc_data, &icc_len)) {
            if (out_icc) {
                *out_icc = icc_data;
            } else if (icc_data) {
                free(icc_data);
            }
            if (out_size) *out_size = (size_t)icc_len;
            ok = TRUE;
        }
    }

    jpeg_destroy_decompress(&cinfo);
    return ok;
}

static void image_attach_icc_from_memory(ImageData *img, const unsigned char *data, size_t size) {
    if (!img || !data || size == 0) return;
    unsigned char *icc = NULL;
    size_t icc_size = 0;
    if (jpeg_extract_icc_from_memory(data, size, &icc, &icc_size)) {
        image_set_icc(img, icc, icc_size);
        free(icc);
    }
}
#endif

static BOOL read_file_buffer(const wchar_t *path, unsigned char **out_buf, size_t *out_size) {
    if (!out_buf || !out_size) return FALSE;
    *out_buf = NULL;
    *out_size = 0;

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER size64;
    if (!GetFileSizeEx(hFile, &size64) || size64.QuadPart <= 0 ||
        (unsigned long long)size64.QuadPart > (size_t)-1) {
        CloseHandle(hFile);
        return FALSE;
    }

    size_t size = (size_t)size64.QuadPart;
    unsigned char *buffer = (unsigned char*)malloc(size);
    if (!buffer) {
        CloseHandle(hFile);
        return FALSE;
    }

    size_t offset = 0;
    while (offset < size) {
        DWORD to_read = (DWORD)((size - offset) > (size_t)0xFFFFFFFFu ? 0xFFFFFFFFu : (size - offset));
        DWORD read = 0;
        if (!ReadFile(hFile, buffer + offset, to_read, &read, NULL) || read == 0) {
            free(buffer);
            CloseHandle(hFile);
            return FALSE;
        }
        offset += read;
    }

    CloseHandle(hFile);
    *out_buf = buffer;
    *out_size = size;
    return TRUE;
}

static BOOL jpeg_decode_to_yuv420_from_memory(const unsigned char *data, size_t size,
                                              JpegYuv420 *out, BOOL *out_err_color) {
    if (!out) return FALSE;
    if (out_err_color) *out_err_color = FALSE;
    memset(out, 0, sizeof(*out));
    if (!data || size == 0) return FALSE;

    tjhandle tj = get_tj_decompress_handle();
    if (!tj) return FALSE;

    int width = 0, height = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, data, (unsigned long)size,
                            &width, &height, &subsamp, &colorspace) < 0) {
        log_tj_yuv_error_once(L"tjDecompressHeader3 (YUV420)", tj);
        return FALSE;
    }

    if (subsamp != TJSAMP_420 || colorspace != TJCS_YCbCr) {
        if (out_err_color) *out_err_color = TRUE;
        return FALSE;
    }

    int y_w = tjPlaneWidth(0, width, subsamp);
    int y_h = tjPlaneHeight(0, height, subsamp);
    int uv_w = tjPlaneWidth(1, width, subsamp);
    int uv_h = tjPlaneHeight(1, height, subsamp);
    if (y_w <= 0 || y_h <= 0 || uv_w <= 0 || uv_h <= 0) {
        return FALSE;
    }

    int y_stride = y_w;
    int uv_stride = uv_w;
    size_t y_size = (size_t)y_stride * (size_t)y_h;
    size_t uv_size = (size_t)uv_stride * (size_t)uv_h;
    size_t total = y_size + uv_size + uv_size;

    unsigned char *buffer = (unsigned char*)malloc(total);
    if (!buffer) return FALSE;

    unsigned char *planes[3];
    int strides[3];
    planes[0] = buffer;
    planes[1] = buffer + y_size;
    planes[2] = buffer + y_size + uv_size;
    strides[0] = y_stride;
    strides[1] = uv_stride;
    strides[2] = uv_stride;

    if (tjDecompressToYUVPlanes(tj, data, (unsigned long)size,
                                planes, width, strides, height, 0) < 0) {
        log_tj_yuv_error_once(L"tjDecompressToYUVPlanes", tj);
        free(buffer);
        return FALSE;
    }

    out->width = width;
    out->height = height;
    out->y_stride = y_stride;
    out->uv_stride = uv_stride;
    out->y_size = y_size;
    out->uv_size = uv_size;
    out->buffer = buffer;
    out->planes[0] = planes[0];
    out->planes[1] = planes[1];
    out->planes[2] = planes[2];
    return TRUE;
}

static BOOL jpeg_decode_to_yuv420(const wchar_t *path, JpegYuv420 *out, BOOL *out_err_color) {
    unsigned char *buffer = NULL;
    size_t size = 0;
    if (!read_file_buffer(path, &buffer, &size)) return FALSE;
    BOOL ok = jpeg_decode_to_yuv420_from_memory(buffer, size, out, out_err_color);
    free(buffer);
    return ok;
}

static void jpeg_yuv420_free(JpegYuv420 *yuv) {
    if (!yuv) return;
    if (yuv->buffer) {
        free(yuv->buffer);
    }
    memset(yuv, 0, sizeof(*yuv));
}

static BOOL jpeg_decode_to_yuv_any_from_memory(const unsigned char *data, size_t size, JpegYuvAny *out) {
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    if (!data || size == 0) return FALSE;

    tjhandle tj = get_tj_decompress_handle();
    if (!tj) return FALSE;

    int width = 0, height = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, data, (unsigned long)size,
                            &width, &height, &subsamp, &colorspace) < 0) {
        log_tj_yuv_error_once(L"tjDecompressHeader3 (YUV)", tj);
        return FALSE;
    }

    int align = 1;
    unsigned long yuv_size_ul = tjBufSizeYUV2(width, align, height, subsamp);
    if (yuv_size_ul == 0 || yuv_size_ul > (unsigned long)(size_t)-1) {
        return FALSE;
    }

    size_t yuv_size = (size_t)yuv_size_ul;
    unsigned char *buffer = (unsigned char*)malloc(yuv_size);
    if (!buffer) return FALSE;

    if (tjDecompressToYUV2(tj, data, (unsigned long)size,
                           buffer, width, align, height, 0) < 0) {
        log_tj_yuv_error_once(L"tjDecompressToYUV2", tj);
        free(buffer);
        return FALSE;
    }

    out->width = width;
    out->height = height;
    out->subsamp = subsamp;
    out->align = align;
    out->size = yuv_size;
    out->buffer = buffer;
    return TRUE;
}

static BOOL jpeg_decode_to_yuv_any(const wchar_t *path, JpegYuvAny *out) {
    unsigned char *buffer = NULL;
    size_t size = 0;
    if (!read_file_buffer(path, &buffer, &size)) return FALSE;
    BOOL ok = jpeg_decode_to_yuv_any_from_memory(buffer, size, out);
    free(buffer);
    return ok;
}

static void jpeg_yuv_any_free(JpegYuvAny *yuv) {
    if (!yuv) return;
    if (yuv->buffer) {
        free(yuv->buffer);
    }
    memset(yuv, 0, sizeof(*yuv));
}

static BOOL jpeg_save_from_yuv_any(const JpegYuvAny *yuv, const wchar_t *path, int quality,
                                   const unsigned char *icc, size_t icc_size) {
    if (!yuv || !yuv->buffer) return FALSE;

    tjhandle tj = get_tj_compress_handle();
    if (!tj) return FALSE;

    unsigned char *jpeg_buf = NULL;
    unsigned long jpeg_size = 0;
    int flags = 0;

    if (tjCompressFromYUV(tj, yuv->buffer, yuv->width, yuv->align, yuv->height,
                          yuv->subsamp, &jpeg_buf, &jpeg_size, quality, flags) < 0) {
        log_tj_yuv_error_once(L"tjCompressFromYUV", tj);
        return FALSE;
    }

    unsigned char *out_buf = jpeg_buf;
    size_t out_size = (size_t)jpeg_size;
    unsigned char *icc_buf = NULL;
    size_t icc_out_size = 0;

    if (icc && icc_size > 0) {
        if (jpeg_insert_icc_profile(jpeg_buf, (size_t)jpeg_size,
                                    icc, icc_size,
                                    &icc_buf, &icc_out_size)) {
            out_buf = icc_buf;
            out_size = icc_out_size;
        }
    }

    FILE *outfile = _wfopen(path, L"wb");
    if (!outfile) {
        if (out_buf != jpeg_buf) free(out_buf);
        tjFree(jpeg_buf);
        return FALSE;
    }

    fwrite(out_buf, 1, out_size, outfile);
    fclose(outfile);

    if (out_buf != jpeg_buf) free(out_buf);
    tjFree(jpeg_buf);
    return TRUE;
}

static BOOL jpeg_insert_icc_profile(const unsigned char *jpeg_buf, size_t jpeg_size,
                                    const unsigned char *icc, size_t icc_size,
                                    unsigned char **out_buf, size_t *out_size) {
    if (out_buf) *out_buf = NULL;
    if (out_size) *out_size = 0;
    if (!jpeg_buf || jpeg_size < 2 || !icc || icc_size == 0) return FALSE;
    if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8) return FALSE;

    size_t num_segments = (icc_size + ICC_MAX_BYTES_IN_MARKER - 1) / ICC_MAX_BYTES_IN_MARKER;
    size_t extra = num_segments * (size_t)(2 + 2 + ICC_OVERHEAD_LEN) + icc_size;
    size_t new_size = jpeg_size + extra;

    unsigned char *out = (unsigned char*)malloc(new_size);
    if (!out) return FALSE;

    size_t insert_pos = 2;
    memcpy(out, jpeg_buf, insert_pos);

    size_t out_pos = insert_pos;
    size_t offset = 0;
    for (size_t seg = 0; seg < num_segments; seg++) {
        size_t chunk = icc_size - offset;
        if (chunk > ICC_MAX_BYTES_IN_MARKER) {
            chunk = ICC_MAX_BYTES_IN_MARKER;
        }

        out[out_pos++] = 0xFF;
        out[out_pos++] = (unsigned char)ICC_MARKER;

        unsigned int length = (unsigned int)(ICC_OVERHEAD_LEN + chunk + 2);
        out[out_pos++] = (unsigned char)((length >> 8) & 0xFF);
        out[out_pos++] = (unsigned char)(length & 0xFF);

        memcpy(out + out_pos, ICC_PROFILE_ID, ICC_PROFILE_ID_LEN - 1);
        out_pos += ICC_PROFILE_ID_LEN - 1;
        out[out_pos++] = '\0';
        out[out_pos++] = (unsigned char)(seg + 1);
        out[out_pos++] = (unsigned char)num_segments;

        memcpy(out + out_pos, icc + offset, chunk);
        out_pos += chunk;
        offset += chunk;
    }

    memcpy(out + out_pos, jpeg_buf + insert_pos, jpeg_size - insert_pos);
    out_pos += jpeg_size - insert_pos;

    if (out_buf) *out_buf = out;
    if (out_size) *out_size = out_pos;
    return TRUE;
}

#ifdef HAVE_WEBP
static BOOL webp_save_from_yuv420(const JpegYuv420 *yuv, const wchar_t *path, int quality) {
    if (!yuv || !yuv->buffer) return FALSE;
    if (quality >= 100) return FALSE;

    WebPConfig config;
    WebPPicture pic;
    WebPMemoryWriter writer;

    if (!WebPConfigInit(&config)) return FALSE;
    config.quality = (float)quality;
    config.method = 2;
    config.pass = 1;
    config.preprocessing = 0;
    config.lossless = 0;

    if (!WebPValidateConfig(&config)) return FALSE;

    if (!WebPPictureInit(&pic)) return FALSE;
    pic.width = yuv->width;
    pic.height = yuv->height;
    pic.use_argb = 0;
    pic.colorspace = WEBP_YUV420;
    pic.y = yuv->planes[0];
    pic.u = yuv->planes[1];
    pic.v = yuv->planes[2];
    pic.y_stride = yuv->y_stride;
    pic.uv_stride = yuv->uv_stride;
    pic.a = NULL;
    pic.a_stride = 0;

    WebPMemoryWriterInit(&writer);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &writer;

    int success = WebPEncode(&config, &pic);
    WebPEncodingError err = pic.error_code;
    WebPPictureFree(&pic);
    if (!success || writer.size == 0) {
        log_webp_yuv_error_once(err);
        WebPMemoryWriterClear(&writer);
        return FALSE;
    }

    FILE *outfile = _wfopen(path, L"wb");
    if (!outfile) {
        WebPMemoryWriterClear(&writer);
        return FALSE;
    }

    fwrite(writer.mem, 1, writer.size, outfile);
    WebPMemoryWriterClear(&writer);
    fclose(outfile);
    return TRUE;
}

#ifdef HAVE_VIPS
BOOL image_convert_jpeg_to_webp_vips(const wchar_t *path, const wchar_t *out_path,
                                     int quality, int effort, int max_resolution, int resolution_percent) {
    if (!image_init_vips()) return FALSE;

    char *in_utf8 = wide_to_utf8_alloc(path);
    if (!in_utf8) return FALSE;
    char *out_utf8 = wide_to_utf8_alloc(out_path);
    if (!out_utf8) {
        free(in_utf8);
        return FALSE;
    }

    VipsImage *src = vips_image_new_from_file(in_utf8,
                                              "access", VIPS_ACCESS_SEQUENTIAL,
                                              NULL);
    if (!src) {
        free(in_utf8);
        free(out_utf8);
        vips_error_clear();
        return FALSE;
    }

    VipsImage *image = src;
    double scale = 1.0;
    int width = vips_image_get_width(src);
    int height = vips_image_get_height(src);

    if (max_resolution > 0) {
        int max_dim = (width > height) ? width : height;
        if (max_dim > max_resolution) {
            scale = (double)max_resolution / (double)max_dim;
        }
    } else if (resolution_percent != 100) {
        scale = (double)resolution_percent / 100.0;
    }

    if (scale != 1.0) {
        if (vips_resize(src, &image, scale,
                        "kernel", VIPS_KERNEL_LINEAR,
                        NULL)) {
            g_object_unref(src);
            free(in_utf8);
            free(out_utf8);
            vips_error_clear();
            return FALSE;
        }
    }

    int lossless = (quality >= 100) ? 1 : 0;
    if (vips_webpsave(image, out_utf8,
                      "Q", quality,
                      "effort", effort,
                      "lossless", lossless,
                      "strip", FALSE,
                      NULL)) {
        if (image != src) g_object_unref(image);
        g_object_unref(src);
        free(in_utf8);
        free(out_utf8);
        vips_error_clear();
        return FALSE;
    }

    if (image != src) g_object_unref(image);
    g_object_unref(src);
    free(in_utf8);
    free(out_utf8);
    return TRUE;
}
#endif

BOOL image_convert_jpeg_to_webp_yuv_fast(const wchar_t *path, const wchar_t *out_path, int quality,
                                         const unsigned char *data, size_t size, int *out_stage) {
    if (out_stage) *out_stage = YUV_FAST_STAGE_NONE;
    if (quality >= 100) return FALSE;

    JpegYuv420 yuv;
    BOOL ok = FALSE;
    if (data && size > 0) {
        ok = jpeg_decode_to_yuv420_from_memory(data, size, &yuv, NULL);
    } else {
        ok = jpeg_decode_to_yuv420(path, &yuv, NULL);
    }
    if (!ok) {
        if (out_stage) *out_stage = YUV_FAST_STAGE_DECODE;
        return FALSE;
    }

    ok = webp_save_from_yuv420(&yuv, out_path, quality);
    jpeg_yuv420_free(&yuv);
    if (!ok) {
        if (out_stage) *out_stage = YUV_FAST_STAGE_ENCODE;
        return FALSE;
    }
    return TRUE;
}
#endif

BOOL image_convert_jpeg_to_jpeg_yuv_fast(const wchar_t *path, const wchar_t *out_path, int quality,
                                         const unsigned char *data, size_t size, int *out_stage) {
    if (out_stage) *out_stage = YUV_FAST_STAGE_NONE;
    
    // Check JPEG magic number before attempting fast-path
    if (data && size > 0) {
        if (!is_valid_jpeg_signature(data, size)) {
            log_error(L"    [Fast-path] %s is not a valid JPEG (magic: 0x%02X 0x%02X), skipping fast-path", 
                      path, data[0], data[1]);
            if (out_stage) *out_stage = YUV_FAST_STAGE_DECODE;
            return FALSE;
        }
    } else {
        // Read file to check magic number if data not provided
        unsigned char *file_buf = NULL;
        size_t file_size = 0;
        if (read_file_buffer(path, &file_buf, &file_size)) {
            if (!is_valid_jpeg_signature(file_buf, file_size)) {
                log_error(L"    [Fast-path] %s is not a valid JPEG (magic: 0x%02X 0x%02X), skipping fast-path", 
                          path, file_buf[0], file_buf[1]);
                free(file_buf);
                if (out_stage) *out_stage = YUV_FAST_STAGE_DECODE;
                return FALSE;
            }
            // Reuse the buffer as data
            data = file_buf;
            size = file_size;
            file_buf = NULL; // Prevent double-free - we'll free via jpeg_free later
        } else {
            free(file_buf);
        }
    }
    
    JpegYuvAny yuv;
    BOOL ok = FALSE;
    unsigned char *icc = NULL;
    size_t icc_size = 0;

    if (data && size > 0) {
        jpeg_extract_icc_from_memory(data, size, &icc, &icc_size);
    }

    if (data && size > 0) {
        ok = jpeg_decode_to_yuv_any_from_memory(data, size, &yuv);
    } else {
        ok = jpeg_decode_to_yuv_any(path, &yuv);
    }
    if (!ok) {
        if (icc) free(icc);
        if (out_stage) *out_stage = YUV_FAST_STAGE_DECODE;
        return FALSE;
    }

    ok = jpeg_save_from_yuv_any(&yuv, out_path, quality, icc, icc_size);
    jpeg_yuv_any_free(&yuv);
    if (icc) free(icc);
    if (!ok) {
        if (out_stage) *out_stage = YUV_FAST_STAGE_ENCODE;
        return FALSE;
    }
    return TRUE;
}

static BOOL image_save_jpeg_turbo(ImageData *img, const wchar_t *path, int quality) {
    tjhandle tj = get_tj_compress_handle();
    if (!tj) return FALSE;

    int pixel_format = TJPF_RGB;
    int subsamp = TJSAMP_420;
    if (img->channels == 1) {
        pixel_format = TJPF_GRAY;
        subsamp = TJSAMP_GRAY;
    } else if (img->channels == 4) {
        pixel_format = TJPF_RGBA;
        subsamp = TJSAMP_420;
    }

    int flags = 0;

    unsigned char *jpeg_buf = NULL;
    unsigned long jpeg_size = 0;
    if (tjCompress2(tj, img->data, img->width, 0, img->height, pixel_format,
                    &jpeg_buf, &jpeg_size, subsamp, quality, flags) < 0) {
        return FALSE;
    }

    unsigned char *out_buf = jpeg_buf;
    size_t out_size = (size_t)jpeg_size;
    unsigned char *icc_buf = NULL;
    size_t icc_size = 0;

    if (img->icc_profile && img->icc_size > 0) {
        if (jpeg_insert_icc_profile(jpeg_buf, (size_t)jpeg_size,
                                    img->icc_profile, img->icc_size,
                                    &icc_buf, &icc_size)) {
            out_buf = icc_buf;
            out_size = icc_size;
        }
    }

    FILE *outfile = _wfopen(path, L"wb");
    if (!outfile) {
        if (out_buf != jpeg_buf) free(out_buf);
        tjFree(jpeg_buf);
        return FALSE;
    }

    fwrite(out_buf, 1, out_size, outfile);
    fclose(outfile);

    if (out_buf != jpeg_buf) free(out_buf);
    tjFree(jpeg_buf);
    return TRUE;
}

static ImageData* image_load_jpeg_turbo(const wchar_t *path) {
    FILE *infile = _wfopen(path, L"rb");
    if (!infile) return NULL;

    if (_fseeki64(infile, 0, SEEK_END) != 0) {
        fclose(infile);
        return NULL;
    }
    __int64 size64 = _ftelli64(infile);
    if (size64 <= 0 || size64 > (__int64)(size_t)-1) {
        fclose(infile);
        return NULL;
    }
    if (_fseeki64(infile, 0, SEEK_SET) != 0) {
        fclose(infile);
        return NULL;
    }

    size_t size = (size_t)size64;
    unsigned char *buffer = (unsigned char*)malloc(size);
    if (!buffer) {
        fclose(infile);
        return NULL;
    }

    size_t read = fread(buffer, 1, size, infile);
    fclose(infile);
    if (read != size) {
        free(buffer);
        return NULL;
    }

    tjhandle tj = get_tj_decompress_handle();
    if (!tj) {
        free(buffer);
        return NULL;
    }

    int width = 0, height = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, buffer, (unsigned long)size,
                            &width, &height, &subsamp, &colorspace) < 0) {
        free(buffer);
        return NULL;
    }

    int pixel_format = (colorspace == TJCS_GRAY) ? TJPF_GRAY : TJPF_RGB;
    int channels = (pixel_format == TJPF_GRAY) ? 1 : 3;

    int pool_index = g_decode_pool_index;
    g_decode_pool_index = (g_decode_pool_index + 1) & 1;
    ImageData *img = image_alloc_or_resize(g_decode_pool[pool_index], width, height, channels, TRUE);
    if (!img) {
        free(buffer);
        return NULL;
    }
    g_decode_pool[pool_index] = img;

    if (tjDecompress2(tj, buffer, (unsigned long)size, img->data,
                      width, 0, height, pixel_format, 0) < 0) {
        free(buffer);
        return NULL;
    }

    free(buffer);
    return img;
}

static ImageData* image_load_jpeg_turbo_from_memory(const unsigned char *buffer, size_t size) {
    if (!buffer || size == 0) return NULL;

    tjhandle tj = get_tj_decompress_handle();
    if (!tj) return NULL;

    int width = 0, height = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, buffer, (unsigned long)size,
                            &width, &height, &subsamp, &colorspace) < 0) {
        return NULL;
    }

    int pixel_format = (colorspace == TJCS_GRAY) ? TJPF_GRAY : TJPF_RGB;
    int channels = (pixel_format == TJPF_GRAY) ? 1 : 3;

    int pool_index = g_decode_pool_index;
    g_decode_pool_index = (g_decode_pool_index + 1) & 1;
    ImageData *img = image_alloc_or_resize(g_decode_pool[pool_index], width, height, channels, TRUE);
    if (!img) {
        return NULL;
    }
    g_decode_pool[pool_index] = img;

    if (tjDecompress2(tj, buffer, (unsigned long)size, img->data,
                      width, 0, height, pixel_format, 0) < 0) {
        return NULL;
    }

    return img;
}

BOOL image_get_jpeg_info(const wchar_t *path, int *out_subsamp, int *out_colorspace) {
    if (out_subsamp) *out_subsamp = -1;
    if (out_colorspace) *out_colorspace = -1;

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_jpeg_info_error_once(L"Failed to open file", GetLastError());
        return FALSE;
    }

    LARGE_INTEGER size64;
    if (!GetFileSizeEx(hFile, &size64) || size64.QuadPart <= 0 ||
        (unsigned long long)size64.QuadPart > (size_t)-1) {
        CloseHandle(hFile);
        log_jpeg_info_error_once(L"Invalid file size", 0);
        return FALSE;
    }

    size_t size = (size_t)size64.QuadPart;
    unsigned char *buffer = (unsigned char*)malloc(size);
    if (!buffer) {
        CloseHandle(hFile);
        log_jpeg_info_error_once(L"Failed to allocate buffer", 0);
        return FALSE;
    }

    size_t offset = 0;
    while (offset < size) {
        DWORD to_read = (DWORD)((size - offset) > (size_t)0xFFFFFFFFu ? 0xFFFFFFFFu : (size - offset));
        DWORD read = 0;
        if (!ReadFile(hFile, buffer + offset, to_read, &read, NULL) || read == 0) {
            DWORD err = GetLastError();
            free(buffer);
            CloseHandle(hFile);
            log_jpeg_info_error_once(L"Failed to read file", err);
            return FALSE;
        }
        offset += read;
    }

    CloseHandle(hFile);

    BOOL ok = image_get_jpeg_info_from_memory(buffer, size, out_subsamp, out_colorspace);
    free(buffer);
    return ok;
}

BOOL image_get_jpeg_info_from_memory(const unsigned char *data, size_t size,
                                     int *out_subsamp, int *out_colorspace) {
    if (out_subsamp) *out_subsamp = -1;
    if (out_colorspace) *out_colorspace = -1;
    if (!data || size == 0) return FALSE;

    // Validate JPEG signature before attempting to parse
    // This prevents crashes on corrupted files with invalid magic numbers
    if (!is_valid_jpeg_signature(data, size)) {
        log_error(L"    image_get_jpeg_info: Not a valid JPEG file (magic: 0x%02X 0x%02X)",
                  data[0], data[1]);
        return FALSE;
    }

    tjhandle tj = get_tj_decompress_handle();
    if (!tj) {
        log_jpeg_info_error_once(L"Failed to init TurboJPEG", 0);
        return FALSE;
    }

    int width = 0, height = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, data, (unsigned long)size,
                            &width, &height, &subsamp, &colorspace) < 0) {
        log_tj_info_error_once(L"tjDecompressHeader3 (info)", tj);
        return FALSE;
    }

    if (out_subsamp) *out_subsamp = subsamp;
    if (out_colorspace) *out_colorspace = colorspace;
    return TRUE;
}
#endif

// JPEG loading using libjpeg-turbo (libjpeg API)
static ImageData* image_load_jpeg(const wchar_t *path, BOOL prefer_rgb) {
    log_message(L"    image_load_jpeg: Loading JPEG: %s", path);

#ifdef HAVE_TURBOJPEG
    if (prefer_rgb) {
        ImageData *turbo = image_load_jpeg_turbo(path);
        if (turbo) {
#ifdef HAVE_JPEG
            unsigned char *file_buf = NULL;
            size_t file_size = 0;
            if (read_file_buffer(path, &file_buf, &file_size)) {
                image_attach_icc_from_memory(turbo, file_buf, file_size);
                free(file_buf);
            }
#endif
            return turbo;
        }
    }
#endif

    FILE *infile = _wfopen(path, L"rb");
    if (!infile) {
        log_error(L"    image_load_jpeg: Failed to open file");
        return NULL;
    }

    struct jpeg_decompress_struct cinfo;
    MyErrorManager jerr;

    // Set up error handling with setjmp/longjmp to catch libjpeg crashes
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_jpeg_error_exit;

    if (setjmp(jerr.jmp_buffer)) {
        // Error occurred during decompression - jump back here
        log_error(L"    image_load_jpeg: JPEG decompression error (corrupted file): %s", path);
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        log_error(L"    image_load_jpeg: Invalid JPEG header");
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return NULL;
    }

    if (prefer_rgb) {
        cinfo.out_color_space = JCS_RGB;
    }

    // Start decompression
    jpeg_start_decompress(&cinfo);

    // Allocate image data
    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int row_stride = cinfo.output_width * cinfo.output_components;
    int out_channels = prefer_rgb ? ((cinfo.output_components == 1) ? 1 : 3) : 4;

    int pool_index = g_decode_pool_index;
    g_decode_pool_index = (g_decode_pool_index + 1) & 1;
    ImageData *img = image_alloc_or_resize(g_decode_pool[pool_index], width, height, out_channels, TRUE);
    if (!img) {
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return NULL;
    }
    g_decode_pool[pool_index] = img;

    // Allocate row buffer (thread-local)
    JSAMPROW row_ptr = (JSAMPROW)jpeg_row_buffer_ensure((size_t)row_stride);
    if (!row_ptr) {
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        image_release(img);
        return NULL;
    }
    JSAMPROW row_buffer[1];
    row_buffer[0] = row_ptr;

    // Read scanlines one at a time
    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, row_buffer, 1);

        JSAMPROW row = row_buffer[0];
        if (cinfo.output_components == 1) {
            if (out_channels == 1) {
                memcpy(&img->data[y * width], row, (size_t)width);
            } else {
                for (int x = 0; x < width; x++) {
                    int dst_idx = (y * width + x) * 4;
                    unsigned char val = row[x];
                    img->data[dst_idx + 0] = val;
                    img->data[dst_idx + 1] = val;
                    img->data[dst_idx + 2] = val;
                    img->data[dst_idx + 3] = 255;
                }
            }
        } else if (cinfo.output_components == 3) {
            if (out_channels == 3) {
                memcpy(&img->data[y * width * 3], row, (size_t)width * 3);
            } else {
                for (int x = 0; x < width; x++) {
                    int dst_idx = (y * width + x) * 4;
                    int src_idx = x * 3;
                    img->data[dst_idx + 0] = row[src_idx + 0]; // R
                    img->data[dst_idx + 1] = row[src_idx + 1]; // G
                    img->data[dst_idx + 2] = row[src_idx + 2]; // B
                    img->data[dst_idx + 3] = 255; // A
                }
            }
        }
        y++;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);

    log_message(L"    image_load_jpeg: JPEG loaded: %dx%d, %d components", width, height, cinfo.output_components);

#ifdef HAVE_JPEG
    {
        unsigned char *file_buf = NULL;
        size_t file_size = 0;
        if (read_file_buffer(path, &file_buf, &file_size)) {
            image_attach_icc_from_memory(img, file_buf, file_size);
            free(file_buf);
        }
    }
#endif
    return img;
}
#endif

#ifdef HAVE_JPEG
ImageData* image_load_jpeg_from_memory(const unsigned char *data, size_t size, BOOL prefer_rgb) {
    if (!data || size == 0) return NULL;

#ifdef HAVE_TURBOJPEG
    if (prefer_rgb) {
        ImageData *turbo = image_load_jpeg_turbo_from_memory(data, size);
        if (turbo) {
#ifdef HAVE_JPEG
            image_attach_icc_from_memory(turbo, data, size);
#endif
            return turbo;
        }
    }
#endif

    struct jpeg_decompress_struct cinfo;
    MyErrorManager jerr;

    // Set up error handling with setjmp/longjmp to catch libjpeg crashes
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_jpeg_error_exit;

    if (setjmp(jerr.jmp_buffer)) {
        // Error occurred during decompression - jump back here
        log_error(L"    image_load_jpeg_from_memory: JPEG decompression error (corrupted data)");
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char*)data, (unsigned long)size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    if (prefer_rgb) {
        cinfo.out_color_space = JCS_RGB;
    }

    jpeg_start_decompress(&cinfo);

    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int row_stride = cinfo.output_width * cinfo.output_components;
    int out_channels = prefer_rgb ? ((cinfo.output_components == 1) ? 1 : 3) : 4;

    int pool_index = g_decode_pool_index;
    g_decode_pool_index = (g_decode_pool_index + 1) & 1;
    ImageData *img = image_alloc_or_resize(g_decode_pool[pool_index], width, height, out_channels, TRUE);
    if (!img) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }
    g_decode_pool[pool_index] = img;

    JSAMPROW row_ptr = (JSAMPROW)jpeg_row_buffer_ensure((size_t)row_stride);
    if (!row_ptr) {
        image_release(img);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }
    JSAMPROW row_buffer[1];
    row_buffer[0] = row_ptr;

    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, row_buffer, 1);
        JSAMPROW row = row_buffer[0];

        if (cinfo.output_components == 1) {
            if (out_channels == 1) {
                memcpy(&img->data[y * width], row, (size_t)width);
            } else {
                for (int x = 0; x < width; x++) {
                    int dst_idx = (y * width + x) * 4;
                    unsigned char val = row[x];
                    img->data[dst_idx + 0] = val;
                    img->data[dst_idx + 1] = val;
                    img->data[dst_idx + 2] = val;
                    img->data[dst_idx + 3] = 255;
                }
            }
        } else if (cinfo.output_components == 3) {
            if (out_channels == 3) {
                memcpy(&img->data[y * width * 3], row, (size_t)width * 3);
            } else {
                for (int x = 0; x < width; x++) {
                    int dst_idx = (y * width + x) * 4;
                    int src_idx = x * 3;
                    img->data[dst_idx + 0] = row[src_idx + 0];
                    img->data[dst_idx + 1] = row[src_idx + 1];
                    img->data[dst_idx + 2] = row[src_idx + 2];
                    img->data[dst_idx + 3] = 255;
                }
            }
        }
        y++;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

#ifdef HAVE_JPEG
    image_attach_icc_from_memory(img, data, size);
#endif
    return img;
}
#endif

#ifdef HAVE_PNG
// PNG loading using libpng
static ImageData* image_load_png(const wchar_t *path, BOOL prefer_rgb) {
    log_message(L"    image_load_png: Loading PNG: %s", path);

    FILE *infile = _wfopen(path, L"rb");
    if (!infile) {
        log_error(L"    image_load_png: Failed to open file");
        return NULL;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        log_error(L"    image_load_png: Failed to create read struct");
        fclose(infile);
        return NULL;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        log_error(L"    image_load_png: Failed to create info struct");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(infile);
        return NULL;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        log_error(L"    image_load_png: Error during PNG read");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(infile);
        return NULL;
    }

    png_init_io(png_ptr, infile);

    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);
    int base_color_type = color_type;
    BOOL has_trns = png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS);
    BOOL has_alpha = has_trns || ((color_type & PNG_COLOR_MASK_ALPHA) != 0);

    // Normalize to RGB or RGBA based on prefer_rgb
    if (base_color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
        base_color_type = PNG_COLOR_TYPE_RGB;
    }
    if (base_color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }

    if (prefer_rgb) {
        // Drop alpha if present and ensure RGB output
        if (base_color_type == PNG_COLOR_TYPE_GRAY || base_color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png_ptr);
            base_color_type = PNG_COLOR_TYPE_RGB;
        }
        if (has_alpha) {
            png_set_strip_alpha(png_ptr);
        }
    } else {
        // Force RGBA output
        if (has_trns) {
            png_set_tRNS_to_alpha(png_ptr);
        }
        if (base_color_type == PNG_COLOR_TYPE_GRAY || base_color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png_ptr);
            base_color_type = PNG_COLOR_TYPE_RGB;
        }
        if ((base_color_type == PNG_COLOR_TYPE_GRAY || base_color_type == PNG_COLOR_TYPE_RGB) && !has_alpha) {
            png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
        }
    }

    // Update info after transformations
    png_read_update_info(png_ptr, info_ptr);

    int channels = png_get_channels(png_ptr, info_ptr);
    ImageData *img = image_alloc(width, height, channels);
    if (!img) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(infile);
        return NULL;
    }

    // Allocate row pointers
    png_bytep *row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++) {
        row_pointers[y] = (png_bytep)&img->data[y * width * channels];
    }

    png_read_image(png_ptr, row_pointers);
    png_read_end(png_ptr, info_ptr);

    free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(infile);

    log_message(L"    image_load_png: PNG loaded: %dx%d, color_type=%d", width, height, color_type);
    return img;
}
#endif

#ifdef HAVE_WEBP
// WebP loading using libwebp
static ImageData* image_load_webp(const wchar_t *path, BOOL prefer_rgb) {
    log_message(L"    image_load_webp: Loading WebP: %s", path);

    // Load WebP file to get data
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_error(L"    image_load_webp: Failed to open file (error: %lu)", GetLastError());
        return NULL;
    }

    DWORD file_size = GetFileSize(hFile, NULL);
    if (file_size == INVALID_FILE_SIZE) {
        log_error(L"    image_load_webp: Failed to get file size");
        CloseHandle(hFile);
        return NULL;
    }

    unsigned char *file_data = (unsigned char*)malloc(file_size);
    if (!file_data) {
        log_error(L"    image_load_webp: Failed to allocate memory for file data");
        CloseHandle(hFile);
        return NULL;
    }

    DWORD bytes_read;
    if (!ReadFile(hFile, file_data, file_size, &bytes_read, NULL) || bytes_read != file_size) {
        log_error(L"    image_load_webp: Failed to read file data");
        free(file_data);
        CloseHandle(hFile);
        return NULL;
    }
    CloseHandle(hFile);

    // Decode WebP
    int width, height;
    uint8_t *decoded_data = prefer_rgb
        ? WebPDecodeRGB(file_data, file_size, &width, &height)
        : WebPDecodeRGBA(file_data, file_size, &width, &height);

    free(file_data);

    if (!decoded_data) {
        log_error(L"    image_load_webp: Failed to decode WebP image");
        return NULL;
    }

    // Allocate and copy image data
    int channels = prefer_rgb ? 3 : 4;
    ImageData *img = image_alloc(width, height, channels);
    if (!img) {
        WebPFree(decoded_data);
        return NULL;
    }

    memcpy(img->data, decoded_data, (size_t)width * height * channels);
    WebPFree(decoded_data);

    log_message(L"    image_load_webp: WebP loaded: %dx%d", width, height);
    return img;
}
#endif

// Main image load function - dispatches to format-specific loaders
ImageData* image_load_ex(const wchar_t *path, BOOL prefer_rgb) {
    log_message(L"    image_load: Loading %s", path);

    ImageFormat format = detect_format_from_path(path);
    log_message(L"    image_load: Detected format: %d", format);

    // Dispatch to format-specific loader
    switch (format) {
        case FORMAT_JPEG:
        #ifdef HAVE_JPEG
            return image_load_jpeg(path, prefer_rgb);
        #else
            log_error(L"    image_load: JPEG support not compiled in");
            return NULL;
        #endif

        case FORMAT_PNG:
        #ifdef HAVE_PNG
            return image_load_png(path, prefer_rgb);
        #else
            log_error(L"    image_load: PNG support not compiled in");
            return NULL;
        #endif

        case FORMAT_WEBP:
        #ifdef HAVE_WEBP
            return image_load_webp(path, prefer_rgb);
        #else
            log_error(L"    image_load: WebP support not compiled in");
            return NULL;
        #endif

        case FORMAT_BMP:
            // Fall through to BMP loader below
            break;

        default:
            log_error(L"    image_load: Unsupported format %d", format);
            return NULL;
    }

    // BMP loader (built-in)
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_error(L"    image_load: Failed to open file (error: %lu)", GetLastError());
        return NULL;
    }

    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    DWORD bytesRead;

    if (!ReadFile(hFile, &bfh, sizeof(bfh), &bytesRead, NULL) || bytesRead != sizeof(bfh)) {
        log_error(L"    image_load: Failed to read file header");
        CloseHandle(hFile);
        return NULL;
    }

    if (bfh.bfType != 0x4D42) {
        log_error(L"    image_load: Not a valid BMP file (type: 0x%X)", bfh.bfType);
        CloseHandle(hFile);
        return NULL;
    }

    if (!ReadFile(hFile, &bih, sizeof(bih), &bytesRead, NULL) || bytesRead != sizeof(bih)) {
        log_error(L"    image_load: Failed to read info header");
        CloseHandle(hFile);
        return NULL;
    }

    int width = bih.biWidth;
    int height = abs(bih.biHeight);
    int channels = 4; // Force RGBA

    ImageData *img = image_alloc(width, height, channels);
    if (!img) {
        CloseHandle(hFile);
        return NULL;
    }

    img->format = format;

    // Read pixel data
    SetFilePointer(hFile, bfh.bfOffBits, NULL, FILE_BEGIN);

    int row_size = ((width * 3 + 3) / 4) * 4; // BMP rows are padded
    unsigned char *row_data = (unsigned char*)malloc(row_size);

    for (int y = height - 1; y >= 0; y--) {
        ReadFile(hFile, row_data, row_size, &bytesRead, NULL);

        for (int x = 0; x < width; x++) {
            int src_idx = x * 3;
            int dst_idx = (y * width + x) * 4;

            // BMP is BGR, convert to RGBA
            img->data[dst_idx + 0] = row_data[src_idx + 2]; // R
            img->data[dst_idx + 1] = row_data[src_idx + 1]; // G
            img->data[dst_idx + 2] = row_data[src_idx + 0]; // B
            img->data[dst_idx + 3] = 255;                    // A
        }
    }

    free(row_data);
    CloseHandle(hFile);

    log_message(L"    image_load: BMP loaded successfully: %dx%d", img->width, img->height);
    return img;
}

ImageData* image_load(const wchar_t *path) {
    return image_load_ex(path, FALSE);
}

// Save image
BOOL image_save(ImageData *img, const wchar_t *path, ImageFormat format, int quality) {
    if (!img || !img->data) {
        log_error(L"    image_save: Invalid image data");
        return FALSE;
    }

    log_message(L"    image_save: Saving to %s as format %d, quality %d", path, format, quality);

    switch (format) {
        case FORMAT_BMP:
            return image_save_bmp(img, path);

        case FORMAT_ICO:
            return image_save_ico(img, path);

        case FORMAT_JPEG:
        #ifdef HAVE_JPEG
            return image_save_jpeg(img, path, quality);
        #else
            return FALSE;
        #endif

        case FORMAT_PNG:
        #ifdef HAVE_PNG
            return image_save_png(img, path, quality);
        #else
            return FALSE;
        #endif

        case FORMAT_WEBP:
        #ifdef HAVE_WEBP
            return image_save_webp(img, path, quality, 0);  // Use fastest effort for slow path
        #else
            return FALSE;
        #endif

        default:
            log_error(L"    image_save: Unknown format %d", format);
            return FALSE;
    }
}

// BMP saving (built-in)
static BOOL image_save_bmp(ImageData *img, const wchar_t *path) {
    log_message(L"      image_save_bmp: Saving %dx%d image to %s", img->width, img->height, path);

    int padding = (4 - (img->width * 3) % 4) % 4;
    int row_size = img->width * 3 + padding;
    int data_size = row_size * img->height;

    BITMAPFILEHEADER bfh = {0};
    BITMAPINFOHEADER bih = {0};

    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + data_size;

    bih.biSize = sizeof(bih);
    bih.biWidth = img->width;
    bih.biHeight = img->height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_error(L"      image_save_bmp: Failed to create file (error: %lu)", GetLastError());
        return FALSE;
    }

    DWORD written;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, NULL);
    WriteFile(hFile, &bih, sizeof(bih), &written, NULL);

    unsigned char *row = (unsigned char*)malloc(row_size);
    memset(row, 0, row_size);

    for (int y = img->height - 1; y >= 0; y--) {
        for (int x = 0; x < img->width; x++) {
            int src_idx = (y * img->width + x) * img->channels;
            int dst_idx = x * 3;

            // Convert to BGR
            row[dst_idx + 0] = img->data[src_idx + 2]; // B
            row[dst_idx + 1] = img->data[src_idx + 1]; // G
            row[dst_idx + 2] = img->data[src_idx + 0]; // R
        }
        WriteFile(hFile, row, row_size, &written, NULL);
    }

    free(row);
    CloseHandle(hFile);

    log_message(L"      image_save_bmp: Save successful");
    return TRUE;
}

// ICO saving (built-in, single 32-bit image)
static BOOL image_save_ico(ImageData *img, const wchar_t *path) {
    if (!img || !img->data) {
        log_error(L"      image_save_ico: Invalid image data");
        return FALSE;
    }

    if (img->width <= 0 || img->height <= 0 || img->width > 256 || img->height > 256) {
        log_error(L"      image_save_ico: Invalid dimensions %dx%d (max 256)", img->width, img->height);
        return FALSE;
    }

    typedef struct {
        WORD reserved;
        WORD type;
        WORD count;
    } ICONDIR;

    typedef struct {
        BYTE width;
        BYTE height;
        BYTE color_count;
        BYTE reserved;
        WORD planes;
        WORD bit_count;
        DWORD bytes_in_res;
        DWORD image_offset;
    } ICONDIRENTRY;

    ICONDIR dir = {0};
    ICONDIRENTRY entry = {0};

    dir.reserved = 0;
    dir.type = 1; // icon
    dir.count = 1;

    entry.width = (img->width == 256) ? 0 : (BYTE)img->width;
    entry.height = (img->height == 256) ? 0 : (BYTE)img->height;
    entry.color_count = 0;
    entry.reserved = 0;
    entry.planes = 1;
    entry.bit_count = 32;

    BITMAPINFOHEADER bih = {0};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = img->width;
    bih.biHeight = img->height * 2; // color + mask
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    int color_size = img->width * img->height * 4;
    int mask_row_bytes = ((img->width + 31) / 32) * 4;
    int mask_size = mask_row_bytes * img->height;
    DWORD image_bytes = sizeof(BITMAPINFOHEADER) + color_size + mask_size;

    entry.bytes_in_res = image_bytes;
    entry.image_offset = sizeof(ICONDIR) + sizeof(ICONDIRENTRY);

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_error(L"      image_save_ico: Failed to create file (error: %lu)", GetLastError());
        return FALSE;
    }

    DWORD bytes_written = 0;
    BOOL ok = TRUE;
    ok &= WriteFile(hFile, &dir, sizeof(dir), &bytes_written, NULL);
    ok &= WriteFile(hFile, &entry, sizeof(entry), &bytes_written, NULL);
    ok &= WriteFile(hFile, &bih, sizeof(bih), &bytes_written, NULL);

    // Write BGRA pixels bottom-up
    unsigned char *row = (unsigned char*)malloc(img->width * 4);
    if (!row) {
        CloseHandle(hFile);
        return FALSE;
    }

    for (int y = img->height - 1; y >= 0 && ok; y--) {
        for (int x = 0; x < img->width; x++) {
            int src_idx = (y * img->width + x) * img->channels;
            unsigned char r = 0, g = 0, b = 0, a = 255;
            if (img->channels == 4) {
                r = img->data[src_idx + 0];
                g = img->data[src_idx + 1];
                b = img->data[src_idx + 2];
                a = img->data[src_idx + 3];
            } else if (img->channels == 3) {
                r = img->data[src_idx + 0];
                g = img->data[src_idx + 1];
                b = img->data[src_idx + 2];
                a = 255;
            } else if (img->channels == 1) {
                r = img->data[src_idx + 0];
                g = r;
                b = r;
                a = 255;
            }

            int dst_idx = x * 4;
            row[dst_idx + 0] = b;
            row[dst_idx + 1] = g;
            row[dst_idx + 2] = r;
            row[dst_idx + 3] = a;
        }
        ok &= WriteFile(hFile, row, img->width * 4, &bytes_written, NULL);
    }

    free(row);

    // AND mask (all zeros = fully opaque)
    if (ok) {
        unsigned char *mask_row = (unsigned char*)calloc(1, (size_t)mask_row_bytes);
        if (!mask_row) {
            CloseHandle(hFile);
            return FALSE;
        }
        for (int y = 0; y < img->height && ok; y++) {
            ok &= WriteFile(hFile, mask_row, mask_row_bytes, &bytes_written, NULL);
        }
        free(mask_row);
    }

    CloseHandle(hFile);

    if (!ok) {
        log_error(L"      image_save_ico: Failed to write file");
        return FALSE;
    }

    log_message(L"      image_save_ico: Save successful");
    return TRUE;
}

#ifdef HAVE_JPEG
static BOOL image_save_jpeg(ImageData *img, const wchar_t *path, int quality) {
#ifdef HAVE_TURBOJPEG
    if (image_save_jpeg_turbo(img, path, quality)) {
        return TRUE;
    }
#endif
    if (!img || !img->data) return FALSE;
    FILE *outfile = _wfopen(path, L"wb");
    if (!outfile) return FALSE;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = img->width;
    cinfo.image_height = img->height;
    if (img->channels == 1) {
        cinfo.input_components = 1;
        cinfo.in_color_space = JCS_GRAYSCALE;
    } else {
        cinfo.input_components = (img->channels == 4) ? 3 : img->channels;
        cinfo.in_color_space = JCS_RGB;
    }

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    if (img->icc_profile && img->icc_size > 0) {
        size_t remaining = img->icc_size;
        size_t offset = 0;
        unsigned char marker_buf[ICC_OVERHEAD_LEN + ICC_MAX_BYTES_IN_MARKER];
        int num_markers = (int)((img->icc_size + ICC_MAX_BYTES_IN_MARKER - 1) / ICC_MAX_BYTES_IN_MARKER);
        for (int i = 0; i < num_markers; i++) {
            size_t chunk = remaining;
            if (chunk > ICC_MAX_BYTES_IN_MARKER) chunk = ICC_MAX_BYTES_IN_MARKER;

            memcpy(marker_buf, ICC_PROFILE_ID, ICC_PROFILE_ID_LEN - 1);
            marker_buf[ICC_PROFILE_ID_LEN - 1] = '\0';
            marker_buf[ICC_PROFILE_ID_LEN] = (unsigned char)(i + 1);
            marker_buf[ICC_PROFILE_ID_LEN + 1] = (unsigned char)num_markers;
            memcpy(marker_buf + ICC_OVERHEAD_LEN, img->icc_profile + offset, chunk);

            jpeg_write_marker(&cinfo, ICC_MARKER, marker_buf,
                              (unsigned int)(ICC_OVERHEAD_LEN + chunk));

            offset += chunk;
            remaining -= chunk;
        }
    }

    unsigned char *row_buffer = NULL;
    if (img->channels == 4) {
        // Convert RGBA to RGB
        row_buffer = jpeg_encode_row_buffer_ensure((size_t)img->width * 3);
        if (!row_buffer) {
            jpeg_finish_compress(&cinfo);
            jpeg_destroy_compress(&cinfo);
            fclose(outfile);
            return FALSE;
        }
    }

    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        if (img->channels == 4) {
            for (int x = 0; x < img->width; x++) {
                int src_idx = (cinfo.next_scanline * img->width + x) * 4;
                int dst_idx = x * 3;
                row_buffer[dst_idx + 0] = img->data[src_idx + 0];
                row_buffer[dst_idx + 1] = img->data[src_idx + 1];
                row_buffer[dst_idx + 2] = img->data[src_idx + 2];
            }
            row_pointer[0] = row_buffer;
        } else {
            row_pointer[0] = &img->data[cinfo.next_scanline * img->width * img->channels];
        }
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(outfile);

    return TRUE;
}
#endif

#ifdef HAVE_PNG
static BOOL image_save_png(ImageData *img, const wchar_t *path, int quality) {
    FILE *outfile = _wfopen(path, L"wb");
    if (!outfile) return FALSE;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(outfile);
        return FALSE;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(outfile);
        return FALSE;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(outfile);
        return FALSE;
    }

    png_init_io(png_ptr, outfile);

    int color_type = (img->channels == 4) ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB;
    png_set_IHDR(png_ptr, info_ptr, img->width, img->height,
                 8, color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    // Compression level based on quality (1-9)
    int compression_level = (quality * 9 + 50) / 100;  // Map 0-100 to 1-9
    png_set_compression_level(png_ptr, compression_level);

    png_write_info(png_ptr, info_ptr);

    png_bytep *row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * img->height);
    for (int y = 0; y < img->height; y++) {
        row_pointers[y] = (png_bytep)&img->data[y * img->width * img->channels];
    }

    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);

    free(row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(outfile);

    return TRUE;
}
#endif

#ifdef HAVE_WEBP
static BOOL image_save_webp(ImageData *img, const wchar_t *path, int quality, int effort) {
    WebPConfig config;
    WebPPicture pic;
    WebPWriteBuffer *writer = &g_webp_writer;

    // Initialize configuration with reasonable defaults
    if (!WebPConfigInit(&config)) return FALSE;

    // Set quality (0-100)
    config.quality = (float)quality;

    // Use method for compression effort (0-6: 0=fastest, 6=slowest/best)
    config.method = effort;

    // Disable special features that increase compression time
    config.pass = 1;  // Single pass for faster encoding 1-10
    config.preprocessing = 0;  // No preprocessing for faster encoding 0 -3
    config.use_sharp_yuv = 1;

    // Use lossless mode only at quality 100
    if (quality >= 100) {
        config.lossless = 1;
    } else {
        config.lossless = 0;
    }

    if (!WebPValidateConfig(&config)) return FALSE;

    // Initialize picture
    if (!WebPPictureInit(&pic)) return FALSE;
    pic.width = img->width;
    pic.height = img->height;

    int import_ok = 0;
    pic.use_argb = (img->channels == 4) ? 1 : 0;
    if (img->channels == 3) {
        import_ok = WebPPictureImportRGB(&pic, img->data, img->width * 3);
    } else if (img->channels == 4) {
        import_ok = WebPPictureImportRGBA(&pic, img->data, img->width * 4);
    } else if (img->channels == 1) {
        size_t pixel_count = (size_t)img->width * img->height;
        size_t buf_size = pixel_count * 3;
        unsigned char *rgb = (unsigned char*)malloc(buf_size);
        if (!rgb) return FALSE;
        for (size_t i = 0; i < pixel_count; i++) {
            unsigned char v = img->data[i];
            rgb[i * 3 + 0] = v;
            rgb[i * 3 + 1] = v;
            rgb[i * 3 + 2] = v;
        }
        import_ok = WebPPictureImportRGB(&pic, rgb, img->width * 3);
        free(rgb);
    } else {
        return FALSE;
    }

    if (!import_ok) {
        WebPPictureFree(&pic);
        return FALSE;
    }

    // Set up thread-local memory writer
    webp_writer_reset(writer);
    pic.writer = webp_write_buffer;
    pic.custom_ptr = writer;

    // Encode
    int success = WebPEncode(&config, &pic);

    WebPPictureFree(&pic);

    if (!success || writer->size == 0) {
        return FALSE;
    }

    const uint8_t *out_bytes = writer->data;
    size_t out_size = writer->size;
    WebPData assembled = {0};

    if (img->icc_profile && img->icc_size > 0) {
        WebPMux *mux = WebPMuxNew();
        if (!mux) {
            return FALSE;
        }
        WebPData image = { writer->data, writer->size };
        if (WebPMuxSetImage(mux, &image, 1) != WEBP_MUX_OK) {
            WebPMuxDelete(mux);
            return FALSE;
        }
        WebPData icc = { img->icc_profile, img->icc_size };
        if (WebPMuxSetChunk(mux, "ICCP", &icc, 1) != WEBP_MUX_OK) {
            WebPMuxDelete(mux);
            return FALSE;
        }
        if (WebPMuxAssemble(mux, &assembled) != WEBP_MUX_OK) {
            WebPMuxDelete(mux);
            return FALSE;
        }
        WebPMuxDelete(mux);
        out_bytes = assembled.bytes;
        out_size = assembled.size;
    }

    FILE *outfile = _wfopen(path, L"wb");
    if (!outfile) {
        if (assembled.bytes) WebPDataClear(&assembled);
        return FALSE;
    }

    fwrite(out_bytes, 1, out_size, outfile);
    fclose(outfile);

    if (assembled.bytes) WebPDataClear(&assembled);

    return TRUE;
}
#endif

int get_optimized_quality(ImageFormat format, int original_quality) {
    switch (format) {
        case FORMAT_WEBP:
            return 85; // WebP at 85% has excellent quality with great compression
        case FORMAT_JPEG:
            return 85; // JPEG at 85% is the sweet spot
        case FORMAT_PNG:
            return 95; // PNG compression level
        default:
            return 85;
    }
}
