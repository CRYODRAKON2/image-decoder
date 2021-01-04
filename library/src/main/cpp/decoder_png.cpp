//
// Created by len on 24/12/20.
//

#include "decoder_png.h"
#include "row_convert.h"
#include <algorithm>

static void png_skip_rows(png_structrp png_ptr, png_uint_32 num_rows) {
  for (png_uint_32 i = 0; i < num_rows; ++i) {
    png_read_row(png_ptr, nullptr, nullptr);
  }
}

bool PngDecoder::handles(const uint8_t* stream) {
  return stream[0] == 0x89 && stream[1] == 0x50 && stream[2] == 0x4E && stream[3] == 0x47;
}

PngDecoder::PngDecoder(
  std::unique_ptr<Stream>&& stream,
  bool cropBorders
) : BaseDecoder(std::move(stream), cropBorders) {
  this->info = parseInfo();
}

PngDecodeSession::PngDecodeSession(Stream* stream) : png(nullptr), pinfo(nullptr),
  reader({ .bytes = stream->bytes, .read = 0, .remain = stream->size }) {
}

void PngDecodeSession::init() {
  auto errorFn = [](png_struct*, png_const_charp msg) {
    throw std::runtime_error(msg);
  };
  auto warnFn = [](png_struct*, png_const_charp msg) {
    LOGW("%s", msg);
  };
  png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, errorFn, warnFn);
  if (!png) {
    throw std::runtime_error("Failed to create png read struct");
  }
  pinfo = png_create_info_struct(png);
  if (!pinfo) {
    throw std::runtime_error("Failed to create png info struct");
  }

  auto readFn = [](png_struct* p, png_byte* data, png_size_t length) {
    auto* r = (PngReader*) png_get_io_ptr(p);
    uint32_t next = std::min(r->remain, (uint32_t) length);
    if (next > 0) {
      memcpy(data, r->bytes + r->read, next);
      r->read += next;
      r->remain -= next;
    }
  };
  png_set_read_fn(png, &reader, readFn);
  png_read_info(png, pinfo);
}

PngDecodeSession::~PngDecodeSession() {
  png_destroy_read_struct(&png, &pinfo, nullptr);
}

std::unique_ptr<PngDecodeSession> PngDecoder::initDecodeSession() {
  auto session = std::make_unique<PngDecodeSession>(stream.get());
  session->init();
  return session;
}

ImageInfo PngDecoder::parseInfo() {
  auto session = initDecodeSession();
  auto png = session->png;
  auto pinfo = session->pinfo;

  uint32_t imageWidth = png_get_image_width(png, pinfo);
  uint32_t imageHeight = png_get_image_height(png, pinfo);

  Rect bounds{};
  if (cropBorders) {
    uint8_t colorType = png_get_color_type(png, pinfo);
    uint8_t bitDepth = png_get_bit_depth(png, pinfo);

    png_set_expand(png);
    if (bitDepth == 16) {
      png_set_scale_16(png);
    }

    if (!(colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)) {
      png_set_rgb_to_gray(png, 1, -1, -1);
    }
    int32_t passes = png_set_interlace_handling(png);

    auto pixels = std::make_unique<uint8_t[]>(imageWidth * imageHeight);
    uint8_t* pixelsPos;
    while (--passes >= 0) {
      pixelsPos = pixels.get();
      for (uint32_t i = 0; i < imageHeight; ++i) {
        png_read_row(png, pixelsPos, nullptr);
        pixelsPos += imageWidth;
      }
    }
    bounds = findBorders(pixels.get(), imageWidth, imageHeight);
  } else {
    bounds = Rect { .x = 0, .y = 0, .width = imageWidth, .height = imageHeight };
  }

  ImageInfo info {
    .imageWidth = imageWidth,
    .imageHeight = imageHeight,
    .isAnimated = false,
    .bounds = bounds
  };

  return info;
}

void PngDecoder::decode(uint8_t* outPixels, Rect outRect, Rect inRect, bool rgb565, uint32_t sampleSize) {
  auto session = initDecodeSession();
  auto png = session->png;
  auto pinfo = session->pinfo;

  uint8_t colorType = png_get_color_type(png, pinfo);
  uint8_t bitDepth = png_get_bit_depth(png, pinfo);

  png_set_expand(png);
  if (bitDepth == 16) {
    png_set_scale_16(png);
  }

  if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }

  if (!(colorType & (uint8_t) PNG_COLOR_MASK_ALPHA)) {
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
  }
  int32_t passes = png_set_interlace_handling(png);

  uint32_t inComponents = 4; // RGB565 is not supported by libpng
  uint32_t inStride = info.imageWidth * inComponents;
  uint32_t inStrideOffset = inRect.x * inComponents;

  uint32_t outStride = outRect.width * (rgb565 ? 2 : 4);

  auto rowFunc = rgb565 ? &RGBA8888_to_RGB565_row : &RGBA8888_to_RGBA8888_row;

  if (sampleSize == 1) {
    auto inRow = std::make_unique<uint8_t[]>(inStride);
    auto* inRowPtr = inRow.get();
    uint8_t* outPixelsPos = nullptr;
    uint32_t inRemainY = info.imageHeight - inRect.height - inRect.y;

    while (--passes >= 0) {
      outPixelsPos = outPixels;
      png_skip_rows(png, inRect.y);
      for (uint32_t i = 0; i < inRect.height; ++i) {
        png_read_row(png, inRowPtr, nullptr);
        rowFunc(outPixelsPos, inRowPtr + inStrideOffset, nullptr, outRect.width, 1);
        outPixelsPos += outStride;
      }
      png_skip_rows(png, inRemainY);
    }
  } else {
    uint32_t skipStart = (sampleSize - 2) / 2;
    uint32_t skipEnd = sampleSize - 2 - skipStart;
    uint32_t inWidthRounded = outRect.width * sampleSize;
    uint32_t inHeightRounded = outRect.height * sampleSize;
    uint32_t inRemainY = info.imageHeight - inHeightRounded - inRect.y;

    if (passes == 1) {
      auto inRow1 = std::make_unique<uint8_t[]>(inStride).get();
      auto inRow2 = std::make_unique<uint8_t[]>(inStride).get();
      uint8_t* outPixelsPos = outPixels;

      png_skip_rows(png, inRect.y);

      for (uint32_t i = 0; i < outRect.height; ++i) {
        png_skip_rows(png, skipStart);
        png_read_row(png, inRow1, nullptr);
        png_read_row(png, inRow2, nullptr);
        rowFunc(outPixelsPos, inRow1 + inStrideOffset, inRow2 + inStrideOffset, outRect.width, sampleSize);
        outPixelsPos += outStride;
        png_skip_rows(png, skipEnd);
      }
    } else {
      auto tmpPixels = std::make_unique<uint8_t[]>(inStride * outRect.height * 2);
      uint8_t* tmpPixelsPos = nullptr;

      while (--passes >= 0) {
        png_skip_rows(png, inRect.y);
        tmpPixelsPos = tmpPixels.get();
        for (uint32_t i = 0; i < outRect.height; ++i) {
          png_skip_rows(png, skipStart);
          png_read_row(png, tmpPixelsPos, nullptr);
          tmpPixelsPos += inStride;
          png_read_row(png, tmpPixelsPos, nullptr);
          tmpPixelsPos += inStride;
          png_skip_rows(png, skipEnd);
        }
        png_skip_rows(png, inRemainY);
      }

      uint8_t* outPixelsPos = outPixels;
      tmpPixelsPos = tmpPixels.get();
      for (uint32_t i = 0; i < outRect.height; ++i) {
        rowFunc(outPixelsPos, tmpPixelsPos + inStrideOffset, tmpPixelsPos + inStride + inStrideOffset,
          outRect.width, sampleSize);
        outPixelsPos += outStride;
        tmpPixelsPos += inStride * 2;
      }
    }
  }
}
