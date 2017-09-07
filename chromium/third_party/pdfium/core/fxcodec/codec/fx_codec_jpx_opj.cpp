// Copyright 2014 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include <algorithm>
#include <limits>
#include <vector>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fxcodec/codec/codec_int.h"
#include "core/fxcodec/fx_codec.h"
#include "core/fxcrt/fx_memory.h"
#include "core/fxcrt/fx_safe_types.h"
#include "third_party/lcms2-2.6/include/lcms2.h"
#include "third_party/libopenjpeg20/openjpeg.h"
#include "third_party/libopenjpeg20/opj_malloc.h"

static void fx_error_callback(const char* msg, void* client_data) {
  (void)client_data;
}

static void fx_warning_callback(const char* msg, void* client_data) {
  (void)client_data;
}

static void fx_info_callback(const char* msg, void* client_data) {
  (void)client_data;
}

OPJ_SIZE_T opj_read_from_memory(void* p_buffer,
                                OPJ_SIZE_T nb_bytes,
                                void* p_user_data) {
  DecodeData* srcData = static_cast<DecodeData*>(p_user_data);
  if (!srcData || !srcData->src_data || srcData->src_size == 0)
    return static_cast<OPJ_SIZE_T>(-1);

  // Reads at EOF return an error code.
  if (srcData->offset >= srcData->src_size)
    return static_cast<OPJ_SIZE_T>(-1);

  OPJ_SIZE_T bufferLength = srcData->src_size - srcData->offset;
  OPJ_SIZE_T readlength = nb_bytes < bufferLength ? nb_bytes : bufferLength;
  memcpy(p_buffer, &srcData->src_data[srcData->offset], readlength);
  srcData->offset += readlength;
  return readlength;
}

OPJ_SIZE_T opj_write_from_memory(void* p_buffer,
                                 OPJ_SIZE_T nb_bytes,
                                 void* p_user_data) {
  DecodeData* srcData = static_cast<DecodeData*>(p_user_data);
  if (!srcData || !srcData->src_data || srcData->src_size == 0) {
    return static_cast<OPJ_SIZE_T>(-1);
  }
  // Writes at EOF return an error code.
  if (srcData->offset >= srcData->src_size) {
    return static_cast<OPJ_SIZE_T>(-1);
  }
  OPJ_SIZE_T bufferLength = srcData->src_size - srcData->offset;
  OPJ_SIZE_T writeLength = nb_bytes < bufferLength ? nb_bytes : bufferLength;
  memcpy(&srcData->src_data[srcData->offset], p_buffer, writeLength);
  srcData->offset += writeLength;
  return writeLength;
}

OPJ_OFF_T opj_skip_from_memory(OPJ_OFF_T nb_bytes, void* p_user_data) {
  DecodeData* srcData = static_cast<DecodeData*>(p_user_data);
  if (!srcData || !srcData->src_data || srcData->src_size == 0)
    return static_cast<OPJ_OFF_T>(-1);

  // Offsets are signed and may indicate a negative skip. Do not support this
  // because of the strange return convention where either bytes skipped or
  // -1 is returned. Following that convention, a successful relative seek of
  // -1 bytes would be required to to give the same result as the error case.
  if (nb_bytes < 0)
    return static_cast<OPJ_OFF_T>(-1);

  // FIXME: use std::make_unsigned<OPJ_OFF_T>::type once c++11 lib is OK'd.
  uint64_t unsignedNbBytes = static_cast<uint64_t>(nb_bytes);
  // Additionally, the offset may take us beyond the range of a size_t (e.g.
  // 32-bit platforms). If so, just clamp at EOF.
  if (unsignedNbBytes >
      std::numeric_limits<OPJ_SIZE_T>::max() - srcData->offset) {
    srcData->offset = srcData->src_size;
  } else {
    OPJ_SIZE_T checkedNbBytes = static_cast<OPJ_SIZE_T>(unsignedNbBytes);
    // Otherwise, mimic fseek() semantics to always succeed, even past EOF,
    // clamping at EOF.  We can get away with this since we don't actually
    // provide negative relative skips from beyond EOF back to inside the
    // data, which would be the only reason to need to know exactly how far
    // beyond EOF we are.
    srcData->offset =
        std::min(srcData->offset + checkedNbBytes, srcData->src_size);
  }
  return nb_bytes;
}

OPJ_BOOL opj_seek_from_memory(OPJ_OFF_T nb_bytes, void* p_user_data) {
  DecodeData* srcData = static_cast<DecodeData*>(p_user_data);
  if (!srcData || !srcData->src_data || srcData->src_size == 0)
    return OPJ_FALSE;

  // Offsets are signed and may indicate a negative position, which would
  // be before the start of the file. Do not support this.
  if (nb_bytes < 0)
    return OPJ_FALSE;

  // FIXME: use std::make_unsigned<OPJ_OFF_T>::type once c++11 lib is OK'd.
  uint64_t unsignedNbBytes = static_cast<uint64_t>(nb_bytes);
  // Additionally, the offset may take us beyond the range of a size_t (e.g.
  // 32-bit platforms). If so, just clamp at EOF.
  if (unsignedNbBytes > std::numeric_limits<OPJ_SIZE_T>::max()) {
    srcData->offset = srcData->src_size;
  } else {
    OPJ_SIZE_T checkedNbBytes = static_cast<OPJ_SIZE_T>(nb_bytes);
    // Otherwise, mimic fseek() semantics to always succeed, even past EOF,
    // again clamping at EOF.
    srcData->offset = std::min(checkedNbBytes, srcData->src_size);
  }
  return OPJ_TRUE;
}

static opj_stream_t* fx_opj_stream_create_memory_stream(
    DecodeData* data,
    OPJ_SIZE_T p_size,
    OPJ_BOOL p_is_read_stream) {
  if (!data || !data->src_data || data->src_size <= 0)
    return nullptr;

  opj_stream_t* stream = opj_stream_create(p_size, p_is_read_stream);
  if (!stream)
    return nullptr;

  opj_stream_set_user_data(stream, data, nullptr);
  opj_stream_set_user_data_length(stream, data->src_size);
  opj_stream_set_read_function(stream, opj_read_from_memory);
  opj_stream_set_skip_function(stream, opj_skip_from_memory);
  opj_stream_set_seek_function(stream, opj_seek_from_memory);
  return stream;
}

static void sycc_to_rgb(int offset,
                        int upb,
                        int y,
                        int cb,
                        int cr,
                        int* out_r,
                        int* out_g,
                        int* out_b) {
  int r, g, b;
  cb -= offset;
  cr -= offset;
  r = y + (int)(1.402 * (float)cr);
  if (r < 0) {
    r = 0;
  } else if (r > upb) {
    r = upb;
  }
  *out_r = r;
  g = y - (int)(0.344 * (float)cb + 0.714 * (float)cr);
  if (g < 0) {
    g = 0;
  } else if (g > upb) {
    g = upb;
  }
  *out_g = g;
  b = y + (int)(1.772 * (float)cb);
  if (b < 0) {
    b = 0;
  } else if (b > upb) {
    b = upb;
  }
  *out_b = b;
}

static void sycc444_to_rgb(opj_image_t* img) {
  int prec = img->comps[0].prec;
  int offset = 1 << (prec - 1);
  int upb = (1 << prec) - 1;
  OPJ_UINT32 maxw =
      std::min({img->comps[0].w, img->comps[1].w, img->comps[2].w});
  OPJ_UINT32 maxh =
      std::min({img->comps[0].h, img->comps[1].h, img->comps[2].h});
  FX_SAFE_SIZE_T max_size = maxw;
  max_size *= maxh;
  if (!max_size.IsValid())
    return;

  const int* y = img->comps[0].data;
  const int* cb = img->comps[1].data;
  const int* cr = img->comps[2].data;
  if (!y || !cb || !cr)
    return;

  int* r = static_cast<int*>(opj_calloc(max_size.ValueOrDie(), sizeof(int)));
  int* g = static_cast<int*>(opj_calloc(max_size.ValueOrDie(), sizeof(int)));
  int* b = static_cast<int*>(opj_calloc(max_size.ValueOrDie(), sizeof(int)));
  int* d0 = r;
  int* d1 = g;
  int* d2 = b;
  for (size_t i = 0; i < max_size.ValueOrDie(); ++i) {
    sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
    ++y;
    ++cb;
    ++cr;
    ++r;
    ++g;
    ++b;
  }
  opj_free(img->comps[0].data);
  opj_free(img->comps[1].data);
  opj_free(img->comps[2].data);
  img->comps[0].data = d0;
  img->comps[1].data = d1;
  img->comps[2].data = d2;
}

static bool sycc420_422_size_is_valid(opj_image_t* img) {
  return (img && img->comps[0].w != std::numeric_limits<OPJ_UINT32>::max() &&
          (img->comps[0].w + 1) / 2 == img->comps[1].w &&
          img->comps[1].w == img->comps[2].w &&
          img->comps[1].h == img->comps[2].h);
}

static bool sycc420_size_is_valid(opj_image_t* img) {
  return (sycc420_422_size_is_valid(img) &&
          img->comps[0].h != std::numeric_limits<OPJ_UINT32>::max() &&
          (img->comps[0].h + 1) / 2 == img->comps[1].h);
}

static bool sycc422_size_is_valid(opj_image_t* img) {
  return (sycc420_422_size_is_valid(img) && img->comps[0].h == img->comps[1].h);
}

static void sycc422_to_rgb(opj_image_t* img) {
  if (!sycc422_size_is_valid(img))
    return;

  int prec = img->comps[0].prec;
  if (prec <= 0 || prec >= 32)
    return;

  int offset = 1 << (prec - 1);
  int upb = (1 << prec) - 1;
  OPJ_UINT32 maxw = img->comps[0].w;
  OPJ_UINT32 maxh = img->comps[0].h;
  FX_SAFE_SIZE_T max_size = maxw;
  max_size *= maxh;
  if (!max_size.IsValid())
    return;

  const int* y = img->comps[0].data;
  const int* cb = img->comps[1].data;
  const int* cr = img->comps[2].data;
  if (!y || !cb || !cr)
    return;

  int* r = static_cast<int*>(opj_calloc(max_size.ValueOrDie(), sizeof(int)));
  int* g = static_cast<int*>(opj_calloc(max_size.ValueOrDie(), sizeof(int)));
  int* b = static_cast<int*>(opj_calloc(max_size.ValueOrDie(), sizeof(int)));
  int* d0 = r;
  int* d1 = g;
  int* d2 = b;
  for (uint32_t i = 0; i < maxh; ++i) {
    OPJ_UINT32 j;
    for (j = 0; j < (maxw & ~static_cast<OPJ_UINT32>(1)); j += 2) {
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      ++cb;
      ++cr;
    }
    if (j < maxw) {
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      ++cb;
      ++cr;
    }
  }
  opj_free(img->comps[0].data);
  opj_free(img->comps[1].data);
  opj_free(img->comps[2].data);
  img->comps[0].data = d0;
  img->comps[1].data = d1;
  img->comps[2].data = d2;
  img->comps[1].w = maxw;
  img->comps[1].h = maxh;
  img->comps[2].w = maxw;
  img->comps[2].h = maxh;
  img->comps[1].dx = img->comps[0].dx;
  img->comps[2].dx = img->comps[0].dx;
  img->comps[1].dy = img->comps[0].dy;
  img->comps[2].dy = img->comps[0].dy;
}

static bool sycc420_must_extend_cbcr(OPJ_UINT32 y, OPJ_UINT32 cbcr) {
  return (y & 1) && (cbcr == y / 2);
}

void sycc420_to_rgb(opj_image_t* img) {
  if (!sycc420_size_is_valid(img))
    return;

  OPJ_UINT32 prec = img->comps[0].prec;
  if (!prec)
    return;

  OPJ_UINT32 offset = 1 << (prec - 1);
  OPJ_UINT32 upb = (1 << prec) - 1;
  OPJ_UINT32 yw = img->comps[0].w;
  OPJ_UINT32 yh = img->comps[0].h;
  OPJ_UINT32 cbw = img->comps[1].w;
  OPJ_UINT32 cbh = img->comps[1].h;
  OPJ_UINT32 crw = img->comps[2].w;
  bool extw = sycc420_must_extend_cbcr(yw, cbw);
  bool exth = sycc420_must_extend_cbcr(yh, cbh);
  FX_SAFE_UINT32 safeSize = yw;
  safeSize *= yh;
  if (!safeSize.IsValid())
    return;

  int* r = static_cast<int*>(opj_calloc(safeSize.ValueOrDie(), sizeof(int)));
  int* g = static_cast<int*>(opj_calloc(safeSize.ValueOrDie(), sizeof(int)));
  int* b = static_cast<int*>(opj_calloc(safeSize.ValueOrDie(), sizeof(int)));
  int* d0 = r;
  int* d1 = g;
  int* d2 = b;
  const int* y = img->comps[0].data;
  const int* cb = img->comps[1].data;
  const int* cr = img->comps[2].data;
  if (!y || !cb || !cr)
    return;

  const int* ny = nullptr;
  int* nr = nullptr;
  int* ng = nullptr;
  int* nb = nullptr;
  OPJ_UINT32 i = 0;
  OPJ_UINT32 j = 0;
  for (i = 0; i < (yh & ~(OPJ_UINT32)1); i += 2) {
    ny = y + yw;
    nr = r + yw;
    ng = g + yw;
    nb = b + yw;
    for (j = 0; j < (yw & ~(OPJ_UINT32)1); j += 2) {
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      sycc_to_rgb(offset, upb, *ny, *cb, *cr, nr, ng, nb);
      ++ny;
      ++nr;
      ++ng;
      ++nb;
      sycc_to_rgb(offset, upb, *ny, *cb, *cr, nr, ng, nb);
      ++ny;
      ++nr;
      ++ng;
      ++nb;
      ++cb;
      ++cr;
    }
    if (j < yw) {
      if (extw) {
        --cb;
        --cr;
      }
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      sycc_to_rgb(offset, upb, *ny, *cb, *cr, nr, ng, nb);
      ++ny;
      ++nr;
      ++ng;
      ++nb;
      ++cb;
      ++cr;
    }
    y += yw;
    r += yw;
    g += yw;
    b += yw;
  }
  if (i < yh) {
    if (exth) {
      cb -= cbw;
      cr -= crw;
    }
    for (j = 0; j < (yw & ~(OPJ_UINT32)1); j += 2) {
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      ++cb;
      ++cr;
    }
    if (j < yw) {
      if (extw) {
        --cb;
        --cr;
      }
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
    }
  }

  opj_free(img->comps[0].data);
  opj_free(img->comps[1].data);
  opj_free(img->comps[2].data);
  img->comps[0].data = d0;
  img->comps[1].data = d1;
  img->comps[2].data = d2;
  img->comps[1].w = yw;
  img->comps[1].h = yh;
  img->comps[2].w = yw;
  img->comps[2].h = yh;
  img->comps[1].w = yw;
  img->comps[1].h = yh;
  img->comps[2].w = yw;
  img->comps[2].h = yh;
  img->comps[1].dx = img->comps[0].dx;
  img->comps[2].dx = img->comps[0].dx;
  img->comps[1].dy = img->comps[0].dy;
  img->comps[2].dy = img->comps[0].dy;
}

static void color_sycc_to_rgb(opj_image_t* img) {
  if (img->numcomps < 3) {
    img->color_space = OPJ_CLRSPC_GRAY;
    return;
  }
  if ((img->comps[0].dx == 1) && (img->comps[1].dx == 2) &&
      (img->comps[2].dx == 2) && (img->comps[0].dy == 1) &&
      (img->comps[1].dy == 2) && (img->comps[2].dy == 2)) {
    sycc420_to_rgb(img);
  } else if ((img->comps[0].dx == 1) && (img->comps[1].dx == 2) &&
             (img->comps[2].dx == 2) && (img->comps[0].dy == 1) &&
             (img->comps[1].dy == 1) && (img->comps[2].dy == 1)) {
    sycc422_to_rgb(img);
  } else if ((img->comps[0].dx == 1) && (img->comps[1].dx == 1) &&
             (img->comps[2].dx == 1) && (img->comps[0].dy == 1) &&
             (img->comps[1].dy == 1) && (img->comps[2].dy == 1)) {
    sycc444_to_rgb(img);
  } else {
    return;
  }
  img->color_space = OPJ_CLRSPC_SRGB;
}

class CJPX_Decoder {
 public:
  explicit CJPX_Decoder(CPDF_ColorSpace* cs);
  ~CJPX_Decoder();
  bool Init(const unsigned char* src_data, uint32_t src_size);
  void GetInfo(uint32_t* width, uint32_t* height, uint32_t* components);
  bool Decode(uint8_t* dest_buf,
              int pitch,
              const std::vector<uint8_t>& offsets);

 private:
  const uint8_t* m_SrcData;
  uint32_t m_SrcSize;
  opj_image_t* image;
  opj_codec_t* l_codec;
  opj_stream_t* l_stream;
  const CPDF_ColorSpace* const m_ColorSpace;
};

CJPX_Decoder::CJPX_Decoder(CPDF_ColorSpace* cs)
    : image(nullptr), l_codec(nullptr), l_stream(nullptr), m_ColorSpace(cs) {}

CJPX_Decoder::~CJPX_Decoder() {
  if (l_codec)
    opj_destroy_codec(l_codec);
  if (l_stream)
    opj_stream_destroy(l_stream);
  if (image)
    opj_image_destroy(image);
}

bool CJPX_Decoder::Init(const unsigned char* src_data, uint32_t src_size) {
  static const unsigned char szJP2Header[] = {
      0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
  if (!src_data || src_size < sizeof(szJP2Header))
    return false;

  image = nullptr;
  m_SrcData = src_data;
  m_SrcSize = src_size;
  DecodeData srcData(const_cast<unsigned char*>(src_data), src_size);
  l_stream = fx_opj_stream_create_memory_stream(&srcData,
                                                OPJ_J2K_STREAM_CHUNK_SIZE, 1);
  if (!l_stream) {
    return false;
  }
  opj_dparameters_t parameters;
  opj_set_default_decoder_parameters(&parameters);
  parameters.decod_format = 0;
  parameters.cod_format = 3;
  if (FXSYS_memcmp(m_SrcData, szJP2Header, sizeof(szJP2Header)) == 0) {
    l_codec = opj_create_decompress(OPJ_CODEC_JP2);
    parameters.decod_format = 1;
  } else {
    l_codec = opj_create_decompress(OPJ_CODEC_J2K);
  }
  if (!l_codec) {
    return false;
  }
  if (m_ColorSpace && m_ColorSpace->GetFamily() == PDFCS_INDEXED)
    parameters.flags |= OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG;
  opj_set_info_handler(l_codec, fx_info_callback, 00);
  opj_set_warning_handler(l_codec, fx_warning_callback, 00);
  opj_set_error_handler(l_codec, fx_error_callback, 00);
  if (!opj_setup_decoder(l_codec, &parameters)) {
    return false;
  }
  if (!opj_read_header(l_stream, l_codec, &image)) {
    image = nullptr;
    return false;
  }
  image->pdfium_use_colorspace = !!m_ColorSpace;

  if (!parameters.nb_tile_to_decode) {
    if (!opj_set_decode_area(l_codec, image, parameters.DA_x0, parameters.DA_y0,
                             parameters.DA_x1, parameters.DA_y1)) {
      opj_image_destroy(image);
      image = nullptr;
      return false;
    }
    if (!(opj_decode(l_codec, l_stream, image) &&
          opj_end_decompress(l_codec, l_stream))) {
      opj_image_destroy(image);
      image = nullptr;
      return false;
    }
  } else {
    if (!opj_get_decoded_tile(l_codec, l_stream, image,
                              parameters.tile_index)) {
      return false;
    }
  }
  opj_stream_destroy(l_stream);
  l_stream = nullptr;
  if (image->color_space != OPJ_CLRSPC_SYCC && image->numcomps == 3 &&
      image->comps[0].dx == image->comps[0].dy && image->comps[1].dx != 1) {
    image->color_space = OPJ_CLRSPC_SYCC;
  } else if (image->numcomps <= 2) {
    image->color_space = OPJ_CLRSPC_GRAY;
  }
  if (image->color_space == OPJ_CLRSPC_SYCC) {
    color_sycc_to_rgb(image);
  }
  if (image->icc_profile_buf) {
    // TODO(palmer): Using |opj_free| here resolves the crash described in
    // https://crbug.com/737033, but ultimately we need to harmonize the
    // memory allocation strategy across OpenJPEG and its PDFium callers.
    opj_free(image->icc_profile_buf);
    image->icc_profile_buf = nullptr;
    image->icc_profile_len = 0;
  }
  return !!image;
}

void CJPX_Decoder::GetInfo(uint32_t* width,
                           uint32_t* height,
                           uint32_t* components) {
  *width = (uint32_t)image->x1;
  *height = (uint32_t)image->y1;
  *components = (uint32_t)image->numcomps;
}

bool CJPX_Decoder::Decode(uint8_t* dest_buf,
                          int pitch,
                          const std::vector<uint8_t>& offsets) {
  if (image->comps[0].w != image->x1 || image->comps[0].h != image->y1)
    return false;

  if (pitch<(int)(image->comps[0].w * 8 * image->numcomps + 31)>> 5 << 2)
    return false;

  FXSYS_memset(dest_buf, 0xff, image->y1 * pitch);
  std::vector<uint8_t*> channel_bufs(image->numcomps);
  std::vector<int> adjust_comps(image->numcomps);
  for (uint32_t i = 0; i < image->numcomps; i++) {
    channel_bufs[i] = dest_buf + offsets[i];
    adjust_comps[i] = image->comps[i].prec - 8;
    if (i > 0) {
      if (image->comps[i].dx != image->comps[i - 1].dx ||
          image->comps[i].dy != image->comps[i - 1].dy ||
          image->comps[i].prec != image->comps[i - 1].prec) {
        return false;
      }
    }
  }
  int width = image->comps[0].w;
  int height = image->comps[0].h;
  for (uint32_t channel = 0; channel < image->numcomps; ++channel) {
    uint8_t* pChannel = channel_bufs[channel];
    if (adjust_comps[channel] < 0) {
      for (int row = 0; row < height; ++row) {
        uint8_t* pScanline = pChannel + row * pitch;
        for (int col = 0; col < width; ++col) {
          uint8_t* pPixel = pScanline + col * image->numcomps;
          if (!image->comps[channel].data)
            continue;

          int src = image->comps[channel].data[row * width + col];
          src += image->comps[channel].sgnd
                     ? 1 << (image->comps[channel].prec - 1)
                     : 0;
          if (adjust_comps[channel] > 0) {
            *pPixel = 0;
          } else {
            *pPixel = (uint8_t)(src << -adjust_comps[channel]);
          }
        }
      }
    } else {
      for (int row = 0; row < height; ++row) {
        uint8_t* pScanline = pChannel + row * pitch;
        for (int col = 0; col < width; ++col) {
          uint8_t* pPixel = pScanline + col * image->numcomps;
          if (!image->comps[channel].data)
            continue;

          int src = image->comps[channel].data[row * width + col];
          src += image->comps[channel].sgnd
                     ? 1 << (image->comps[channel].prec - 1)
                     : 0;
          if (adjust_comps[channel] - 1 < 0) {
            *pPixel = (uint8_t)((src >> adjust_comps[channel]));
          } else {
            int tmpPixel = (src >> adjust_comps[channel]) +
                           ((src >> (adjust_comps[channel] - 1)) % 2);
            if (tmpPixel > 255) {
              tmpPixel = 255;
            } else if (tmpPixel < 0) {
              tmpPixel = 0;
            }
            *pPixel = (uint8_t)tmpPixel;
          }
        }
      }
    }
  }
  return true;
}

CCodec_JpxModule::CCodec_JpxModule() {}
CCodec_JpxModule::~CCodec_JpxModule() {}

CJPX_Decoder* CCodec_JpxModule::CreateDecoder(const uint8_t* src_buf,
                                              uint32_t src_size,
                                              CPDF_ColorSpace* cs) {
  std::unique_ptr<CJPX_Decoder> decoder(new CJPX_Decoder(cs));
  return decoder->Init(src_buf, src_size) ? decoder.release() : nullptr;
}

void CCodec_JpxModule::GetImageInfo(CJPX_Decoder* pDecoder,
                                    uint32_t* width,
                                    uint32_t* height,
                                    uint32_t* components) {
  pDecoder->GetInfo(width, height, components);
}

bool CCodec_JpxModule::Decode(CJPX_Decoder* pDecoder,
                              uint8_t* dest_data,
                              int pitch,
                              const std::vector<uint8_t>& offsets) {
  return pDecoder->Decode(dest_data, pitch, offsets);
}

void CCodec_JpxModule::DestroyDecoder(CJPX_Decoder* pDecoder) {
  delete pDecoder;
}
