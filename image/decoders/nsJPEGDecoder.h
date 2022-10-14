/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_decoders_nsJPEGDecoder_h
#define mozilla_image_decoders_nsJPEGDecoder_h

#include "RasterImage.h"
#include "SurfacePipe.h"
#include "EXIF.h"

// On Windows systems, RasterImage.h brings in 'windows.h', which defines INT32.
// But the jpeg decoder has its own definition of INT32. To avoid build issues,
// we need to undefine the version from 'windows.h'.
#undef INT32

#include "Decoder.h"

extern "C" {
#include "jpeglib.h"
}

#include <setjmp.h>
#include <memory>

namespace mozilla::image {

typedef struct {
  struct jpeg_error_mgr pub;  // "public" fields for IJG library
  // jmp_buf setjmp_buffer;      // For handling catastropic errors
} decoder_error_mgr;

}  // namespace mozilla::image


typedef mozilla::image::decoder_error_mgr decoder_error_mgr;

#include "JpegRLBoxTypes.h"

namespace mozilla::image {
typedef enum {
  JPEG_HEADER,  // Reading JFIF headers
  JPEG_START_DECOMPRESS,
  JPEG_DECOMPRESS_PROGRESSIVE,  // Output progressive pixels
  JPEG_DECOMPRESS_SEQUENTIAL,   // Output sequential pixels
  JPEG_DONE,
  JPEG_SINK_NON_JPEG_TRAILER,  // Some image files have a
                               // non-JPEG trailer
  JPEG_ERROR
} jstate;

class RasterImage;
struct Orientation;

class nsJPEGDecoder : public Decoder {
 public:
  virtual ~nsJPEGDecoder();

  DecoderType GetType() const override { return DecoderType::JPEG; }

  void NotifyDone();

 protected:
  nsresult InitInternal() override;
  LexerResult DoDecode(SourceBufferIterator& aIterator,
                       IResumable* aOnResume) override;
  nsresult FinishInternal() override;

  Maybe<Telemetry::HistogramID> SpeedHistogram() const override;

 protected:
  EXIFData ReadExifData() const;
  WriteState OutputScanlines();

 private:
  friend class DecoderFactory;

  // Decoders should only be instantiated via DecoderFactory.
  nsJPEGDecoder(RasterImage* aImage, Decoder::DecodeStyle aDecodeStyle, RasterImage* aImageExtra);

  enum class State { JPEG_DATA, FINISHED_JPEG_DATA };

  void FinishRow(uint32_t aLastSourceRow);
  LexerTransition<State> ReadJPEGData(const char* aData, size_t aLength);
  LexerTransition<State> FinishedJPEGData();

  StreamingLexer<State> mLexer;

 public:
  rlbox_sandbox_jpeg* mSandbox;

  tainted_opaque_jpeg<unsigned char*> transfer_input_bytes(
    unsigned char* buffer, size_t size,
    tainted_opaque_jpeg<unsigned char*>& transfer_buffer,
    size_t& transfer_buffer_size);

  tainted_opaque_jpeg<unsigned char*> transfer_input_bytes(
    unsigned char* buffer, size_t size,
    tainted_opaque_jpeg<unsigned char*>& transfer_buffer,
    size_t& transfer_buffer_size,
    bool& used_copy);

 private:
  void getRLBoxSandbox();
  void releaseRLBoxSandbox();
  sandbox_callback_jpeg<void(*)(jpeg_decompress_struct *)>* m_init_source_cb;
  sandbox_callback_jpeg<void(*)(j_decompress_ptr)>* m_term_source_cb;
  sandbox_callback_jpeg<void(*)(j_decompress_ptr, long)>* m_skip_input_data_cb;
  sandbox_callback_jpeg<boolean(*)(j_decompress_ptr)>* m_fill_input_buffer_cb;
  sandbox_callback_jpeg<void(*)(j_common_ptr)>* m_my_error_exit_cb;
  sandbox_callback_jpeg<void(*)(j_common_ptr)>* m_progress_monitor_cb;

  std::string mImageString;
 public:
  tainted_opaque_jpeg<unsigned char*> m_input_transfer_buffer;
  size_t m_input_transfer_buffer_size;
  tainted_opaque_jpeg<unsigned char*> m_output_transfer_buffer;
  size_t m_output_transfer_buffer_size;

  tainted_opaque_jpeg<unsigned char**> m_p_output_transfer_buffer;

  jmp_buf m_jmpBuff;
  bool m_jmpBuffValid = false;

  tainted_opaque_jpeg<jpeg_decompress_struct*> p_mInfo;
  tainted_opaque_jpeg<jpeg_source_mgr*> p_mSourceMgr;
  tainted_opaque_jpeg<jpeg_progress_mgr*> p_mProgressMgr;
  tainted_opaque_jpeg<decoder_error_mgr*> p_mErr;
  jstate mState;

  uint32_t mBytesToSkip;

  const JOCTET* mSegment;  // The current segment we are decoding from
  uint32_t mSegmentLen;    // amount of data in mSegment

  JOCTET* mBackBuffer;
  uint32_t mBackBufferLen;   // Offset of end of active backtrack data
  uint32_t mBackBufferSize;  // size in bytes what mBackBuffer was created with
  uint32_t mBackBufferUnreadLen;  // amount of data currently in mBackBuffer

  JOCTET* mProfile;
  uint32_t mProfileLength;

  uint32_t* mCMSLine;

  bool mReading;

  const Decoder::DecodeStyle mDecodeStyle;

  SurfacePipe mPipe;
};

}  // namespace mozilla::image

#endif  // mozilla_image_decoders_nsJPEGDecoder_h
