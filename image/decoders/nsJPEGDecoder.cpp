/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ImageLogging.h"  // Must appear first.

#include "nsJPEGDecoder.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

#include "imgFrame.h"
#include "Orientation.h"
#include "EXIF.h"
#include "SurfacePipeFactory.h"

#include "nspr.h"
#include "nsCRT.h"
#include "gfxColor.h"

#include "jerror.h"

#include "gfxPlatform.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/Telemetry.h"

#include "JpegRLBox.h"

extern "C" {
#include "iccjpeg.h"
}

#if MOZ_BIG_ENDIAN()
#  define MOZ_JCS_EXT_NATIVE_ENDIAN_XRGB JCS_EXT_XRGB
#else
#  define MOZ_JCS_EXT_NATIVE_ENDIAN_XRGB JCS_EXT_BGRX
#endif

static void cmyk_convert_bgra(uint32_t* aInput, uint32_t* aOutput,
                              int32_t aWidth);

using mozilla::gfx::SurfaceFormat;

static inline constexpr char RLBOX_JPEG_STATE_ASSERTION[] =
    "Tainted data is being inspected only to check the internal state of "
    "libjpeg structures. This is not a condition that is critical for safety of "
    "the renderer.";

static inline constexpr char RLBOX_JPEG_TAINTED_ASSERTION[] =
    "Only checked to change the value of other tainted variables.";

static std::atomic<unsigned int> g_rendered_jpeg_count = 1;
namespace mozilla {
namespace image {

static mozilla::LazyLogModule sJPEGLog("JPEGDecoder");

static mozilla::LazyLogModule sJPEGDecoderAccountingLog(
    "JPEGDecoderAccounting");

static thread_local void* jpegRendererSaved = nullptr;

static qcms_profile* GetICCProfile(tainted_volatile_jpeg<jpeg_decompress_struct>& info) {
  JOCTET* profilebuf;
  uint32_t profileLength;
  qcms_profile* profile = nullptr;

  if (read_icc_profile((&info).to_opaque(), &profilebuf, &profileLength)) {
    profile = qcms_profile_from_memory(profilebuf, profileLength);
    free(profilebuf);
  }

  return profile;
}

METHODDEF(void) init_source(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd);
METHODDEF(tainted_jpeg<boolean>) fill_input_buffer(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd);
METHODDEF(void) skip_input_data(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd, tainted_jpeg<long> num_bytes);
METHODDEF(void) term_source(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd);
METHODDEF(void) my_error_exit(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_common_ptr> cinfo);
METHODDEF(void) progress_monitor(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_common_ptr> info);

// Normal JFIF markers can't have more bytes than this.
#define MAX_JPEG_MARKER_LENGTH (((uint32_t)1 << 16) - 1)

struct nsJPEGDecoderSandboxData {
  bool used;
  rlbox_sandbox_jpeg sandbox;
  sandbox_callback_jpeg<void(*)(jpeg_decompress_struct *)> init_source_cb;
  sandbox_callback_jpeg<void(*)(j_decompress_ptr)> term_source_cb;
  sandbox_callback_jpeg<void(*)(j_decompress_ptr, long)> skip_input_data_cb;
  sandbox_callback_jpeg<boolean(*)(j_decompress_ptr)> fill_input_buffer_cb;
  sandbox_callback_jpeg<void(*)(j_common_ptr)> my_error_exit_cb;
  sandbox_callback_jpeg<void(*)(j_common_ptr)> progress_monitor_cb;

  nsJPEGDecoderSandboxData() {
    used = false;
    sandbox.create_sandbox();
    init_source_cb = sandbox.register_callback(init_source);
    term_source_cb = sandbox.register_callback(term_source);
    skip_input_data_cb = sandbox.register_callback(skip_input_data);
    fill_input_buffer_cb = sandbox.register_callback(fill_input_buffer);
    my_error_exit_cb = sandbox.register_callback(my_error_exit);
    progress_monitor_cb = sandbox.register_callback(progress_monitor);
  }
};

static nsJPEGDecoderSandboxData* chosenSandbox = nullptr;
static std::mutex jpeg_sandbox_create_mutex;

void nsJPEGDecoder::getRLBoxSandbox() {
  std::lock_guard<std::mutex> lock(jpeg_sandbox_create_mutex);
  if (chosenSandbox == nullptr) {
    chosenSandbox = new nsJPEGDecoderSandboxData();
  } else {
    while (chosenSandbox->used == true) {
      jpeg_sandbox_create_mutex.unlock();
      usleep(100);
      jpeg_sandbox_create_mutex.lock();
    }
  }

  chosenSandbox->used = true;
  mSandbox = &(chosenSandbox->sandbox);
  m_init_source_cb = &(chosenSandbox->init_source_cb);
  m_term_source_cb = &(chosenSandbox->term_source_cb);
  m_skip_input_data_cb = &(chosenSandbox->skip_input_data_cb);
  m_fill_input_buffer_cb = &(chosenSandbox->fill_input_buffer_cb);
  m_my_error_exit_cb = &(chosenSandbox->my_error_exit_cb);
  m_progress_monitor_cb = &(chosenSandbox->progress_monitor_cb);
}

void nsJPEGDecoder::releaseRLBoxSandbox()
{
  mSandbox = nullptr;
  m_init_source_cb = nullptr;
  m_term_source_cb = nullptr;
  m_skip_input_data_cb = nullptr;
  m_fill_input_buffer_cb = nullptr;
  m_my_error_exit_cb = nullptr;

  std::lock_guard<std::mutex> lock2(jpeg_sandbox_create_mutex);
  chosenSandbox->used = false;
}

inline std::string getImageURIString(RasterImage* aImage)
{
  nsIURI* imageURI = nullptr;

  //Try to retrieve the image URI from the ImageDecoder request
  if(aImage != nullptr) {
    imageURI = aImage->GetURI();
  }

  //if still null bail out - empty string causes the use of a temporary sandbox
  if(imageURI == nullptr) { return ""; }

  nsCString spec;
  imageURI->GetSpec(spec);
  std::string ret = spec.get();
  return ret;
}

nsJPEGDecoder::nsJPEGDecoder(RasterImage* aImage,
                             Decoder::DecodeStyle aDecodeStyle,
                             RasterImage* aImageExtra)
    : Decoder(aImage),
      mLexer(Transition::ToUnbuffered(State::FINISHED_JPEG_DATA,
                                      State::JPEG_DATA, SIZE_MAX),
             Transition::TerminateSuccess()),
      mProfile(nullptr),
      mProfileLength(0),
      mCMSLine(nullptr),
      mDecodeStyle(aDecodeStyle) {
  mImageString = getImageURIString(aImage != nullptr? aImage : aImageExtra);
  getRLBoxSandbox();
  auto mInfo_obj = mSandbox->malloc_in_sandbox<jpeg_decompress_struct>();
  auto& mInfo = *mInfo_obj;
  auto mSourceMgr_obj = mSandbox->malloc_in_sandbox<jpeg_source_mgr>();
  auto& mSourceMgr = *mSourceMgr_obj;
  auto mErr_obj = mSandbox->malloc_in_sandbox<decoder_error_mgr>();
  auto& mErr = *mErr_obj;
  auto mProgressMgr_obj = mSandbox->malloc_in_sandbox<jpeg_progress_mgr>();
  auto& mProgressMgr = *mProgressMgr_obj;

  this->p_mInfo = mInfo_obj.to_opaque();
  this->p_mSourceMgr = mSourceMgr_obj.to_opaque();
  this->p_mErr = mErr_obj.to_opaque();
  mErr.pub.error_exit = nullptr;
  mErr.pub.emit_message = nullptr;
  mErr.pub.output_message = nullptr;
  mErr.pub.format_message = nullptr;
  mErr.pub.reset_error_mgr = nullptr;
  mErr.pub.msg_code = 0;
  mErr.pub.trace_level = 0;
  mErr.pub.num_warnings = 0;
  mErr.pub.jpeg_message_table = nullptr;
  mErr.pub.last_jpeg_message = 0;
  mErr.pub.addon_message_table = nullptr;
  mErr.pub.first_addon_message = 0;
  mErr.pub.last_addon_message = 0;
  mState = JPEG_HEADER;
  mReading = true;
  mImageData = nullptr;

  mBytesToSkip = 0;
  rlbox::memset(*mSandbox, &mInfo, 0, sizeof(mInfo));
  rlbox::memset(*mSandbox, &mSourceMgr, 0, sizeof(mSourceMgr));
  rlbox::memset(*mSandbox, &mProgressMgr, 0, sizeof(mProgressMgr));
  mInfo.client_data = nullptr;

  mSegment = nullptr;
  mSegmentLen = 0;

  mBackBuffer = nullptr;
  mBackBufferLen = mBackBufferSize = mBackBufferUnreadLen = 0;

  m_input_transfer_buffer.set_zero();
  m_input_transfer_buffer_size = 0;
  m_output_transfer_buffer.set_zero();
  m_output_transfer_buffer_size = 0;

  auto p_output_transfer_buffer = mSandbox->malloc_in_sandbox<unsigned char*>();
  m_p_output_transfer_buffer = p_output_transfer_buffer.to_opaque();

  MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
          ("nsJPEGDecoder::nsJPEGDecoder: Creating JPEG decoder %p", this));
}

nsJPEGDecoder::~nsJPEGDecoder() {
  auto& mInfo = *rlbox::from_opaque(p_mInfo);
  // Step 8: Release JPEG decompression object
  mInfo.src = nullptr;
  sandbox_invoke(*mSandbox, jpeg_destroy_decompress, &mInfo);

  if (m_input_transfer_buffer_size != 0) {
    mSandbox->free_in_sandbox(m_input_transfer_buffer);
    m_input_transfer_buffer.set_zero();
  }
  if (m_output_transfer_buffer_size != 0) {
    mSandbox->free_in_sandbox(m_output_transfer_buffer);
    m_output_transfer_buffer.set_zero();
  }

  mSandbox->free_in_sandbox(m_p_output_transfer_buffer);
  mSandbox->free_in_sandbox(p_mInfo);
  mSandbox->free_in_sandbox(p_mSourceMgr);
  mSandbox->free_in_sandbox(p_mErr);
  mSandbox->free_in_sandbox(p_mProgressMgr);
  m_p_output_transfer_buffer.set_zero();
  p_mInfo.set_zero();
  p_mSourceMgr.set_zero();
  p_mErr.set_zero();

  if (!IsMetadataDecode()) {
    auto jpeg_count = g_rendered_jpeg_count++;

    // auto num_fncalls = 0;
    // auto num_callbacks = 0;
    // auto& transition_times = mSandbox->process_and_get_transition_times();
    // for (auto& transition_time : transition_times) {
    //   if (transition_time.invoke == rlbox::rlbox_transition::INVOKE) {
    //     num_fncalls++;
    //   } else {
    //     num_callbacks++;
    //   }
    // }

#ifdef RLBOX_MEASURE_TRANSITION_TIMES
    auto time_ns = mSandbox->get_total_ns_time_in_sandbox_and_transitions();
#else
    auto time_ns = 0;
#endif
    std::string tag = "JPEG_destroy(" + mImageString + ")";
    printf("Capture_Time:%s,%u,%ld|\n", tag.c_str(), jpeg_count, (long int) time_ns);
    // printf("Fn calls: %d, Callbacks: %d\n", num_fncalls, num_callbacks);
  }

  releaseRLBoxSandbox();

  free(mBackBuffer);
  mBackBuffer = nullptr;

  delete[] mCMSLine;

  MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
          ("nsJPEGDecoder::~nsJPEGDecoder: Destroying JPEG decoder %p", this));
}

Maybe<Telemetry::HistogramID> nsJPEGDecoder::SpeedHistogram() const {
  return Some(Telemetry::IMAGE_DECODE_SPEED_JPEG);
}

nsresult nsJPEGDecoder::InitInternal() {
  auto& mInfo = *rlbox::from_opaque(p_mInfo);
  auto& mSourceMgr = *rlbox::from_opaque(p_mSourceMgr);
  auto& mErr = *rlbox::from_opaque(p_mErr);
  auto& mProgressMgr = *rlbox::from_opaque(p_mProgressMgr);

  // We set up the normal JPEG error routines, then override error_exit.
  mInfo.err = sandbox_invoke(*mSandbox, jpeg_std_error, &mErr.pub);
  //   mInfo.err = jpeg_std_error(&mErr.pub);
  mErr.pub.error_exit = *m_my_error_exit_cb;
  // Establish the setjmp return context for my_error_exit to use.
  if (setjmp(m_jmpBuff)) {
    // If we get here, the JPEG code has signaled an error, and initialization
    // has failed.
    return NS_ERROR_FAILURE;
  }

  m_jmpBuffValid = true;
  // Step 1: allocate and initialize JPEG decompression object
  sandbox_invoke(*mSandbox, jpeg_CreateDecompress, &mInfo, JPEG_LIB_VERSION, sizeof(mInfo));
  // Set the source manager
  mInfo.src = &mSourceMgr;

  // Step 2: specify data source (eg, a file)

  // Setup callback functions.
  mSourceMgr.init_source = *m_init_source_cb;
  mSourceMgr.fill_input_buffer = *m_fill_input_buffer_cb;
  mSourceMgr.skip_input_data = *m_skip_input_data_cb;
  mSourceMgr.resync_to_restart = mSandbox->get_sandbox_function_address(jpeg_resync_to_restart);
  mSourceMgr.term_source = *m_term_source_cb;

  mInfo.mem->max_memory_to_use = static_cast<long>(
      std::min<size_t>(SurfaceCache::MaximumCapacity(), LONG_MAX));

  mProgressMgr.progress_monitor = *m_progress_monitor_cb;
  mInfo.progress = &mProgressMgr;

  // Record app markers for ICC data
  for (uint32_t m = 0; m < 16; m++) {
    sandbox_invoke(*mSandbox, jpeg_save_markers, &mInfo, JPEG_APP0 + m, 0xFFFF);
  }

  return NS_OK;
}

nsresult nsJPEGDecoder::FinishInternal() {
  // If we're not in any sort of error case, force our state to JPEG_DONE.
  if ((mState != JPEG_DONE && mState != JPEG_SINK_NON_JPEG_TRAILER) &&
      (mState != JPEG_ERROR) && !IsMetadataDecode()) {
    mState = JPEG_DONE;
  }

  jpegRendererSaved = nullptr;
  return NS_OK;
}

LexerResult nsJPEGDecoder::DoDecode(SourceBufferIterator& aIterator,
                                    IResumable* aOnResume) {
  MOZ_ASSERT(!HasError(), "Shouldn't call DoDecode after error!");

  return mLexer.Lex(aIterator, aOnResume,
                    [=](State aState, const char* aData, size_t aLength) {
                      switch (aState) {
                        case State::JPEG_DATA:
                          return ReadJPEGData(aData, aLength);
                        case State::FINISHED_JPEG_DATA:
                          return FinishedJPEGData();
                      }
                      MOZ_CRASH("Unknown State");
                    });
}

LexerTransition<nsJPEGDecoder::State> nsJPEGDecoder::ReadJPEGData(
    const char* aData, size_t aLength) {
  jpegRendererSaved = this;
  auto& mInfo = *rlbox::from_opaque(p_mInfo);
  mSegment = reinterpret_cast<const JOCTET*>(aData);
  mSegmentLen = aLength;

  // Return here if there is a error within libjpeg.
  nsresult error_code;
  // This cast to nsresult makes sense because setjmp() returns whatever we
  // passed to longjmp(), which was actually an nsresult. These error codes
  // have been translated from libjpeg error codes, like so:
  // JERR_OUT_OF_MEMORY => NS_ERROR_OUT_OF_MEMORY
  // JERR_UNKNOWN_MARKER => NS_ERROR_ILLEGAL_VALUE
  // JERR_SOF_UNSUPPORTED =>  NS_ERROR_INVALID_CONTENT_ENCODING
  // <any other error> => NS_ERROR_FAILURE
  if ((error_code = static_cast<nsresult>(setjmp(m_jmpBuff))) !=
      NS_OK) {
    bool fatal = true;
    if (error_code == NS_ERROR_FAILURE) {
      // Error due to corrupt data. Make sure that we don't feed any more data
      // to libjpeg-turbo.
      mState = JPEG_SINK_NON_JPEG_TRAILER;
      MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
              ("} (setjmp returned NS_ERROR_FAILURE)"));
    } else if (error_code == NS_ERROR_ILLEGAL_VALUE) {
      // This is a recoverable error. Consume the marker and continue.
      mInfo.unread_marker = 0;
      fatal = false;
    } else if (error_code == NS_ERROR_INVALID_CONTENT_ENCODING) {
      // The content is encoding frames with a format that libjpeg can't handle.
      MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
              ("} (setjmp returned NS_ERROR_INVALID_CONTENT_ENCODING)"));
      // Check to see if we're in the done state, which indicates that we've
      // already processed the main JPEG data.
      bool inDoneState = (mState == JPEG_DONE);
      // Whether we succeed or fail, we shouldn't send any more data.
      mState = JPEG_SINK_NON_JPEG_TRAILER;

      // If we're in the done state, we exit successfully and attempt to
      // display the content we've already received. Otherwise, we fallthrough
      // and treat this as a fatal error.
      if (inDoneState) {
        return Transition::TerminateSuccess();
      }
    } else {
      // Error for another reason. (Possibly OOM.)
      mState = JPEG_ERROR;
      MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
              ("} (setjmp returned an error)"));
    }

    if (fatal) {
      return Transition::TerminateFailure();
    }
  }

  m_jmpBuffValid = true;

  MOZ_LOG(sJPEGLog, LogLevel::Debug,
          ("[this=%p] nsJPEGDecoder::Write -- processing JPEG data\n", this));

  switch (mState) {
    case JPEG_HEADER: {
      LOG_SCOPE((mozilla::LogModule*)sJPEGLog,
                "nsJPEGDecoder::Write -- entering JPEG_HEADER"
                " case");

      auto status = sandbox_invoke(*mSandbox, jpeg_read_header, &mInfo, TRUE);
      // Step 3: read file parameters with jpeg_read_header()
      if (status.unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION) == JPEG_SUSPENDED) {
        MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                ("} (JPEG_SUSPENDED)"));
        return Transition::ContinueUnbuffered(
            State::JPEG_DATA);  // I/O suspension
      }

      // Post our size to the superclass
      EXIFData exif = ReadExifData();
      PostSize(mInfo.image_width.UNSAFE_unverified(), mInfo.image_height.UNSAFE_unverified(), exif.orientation,
               exif.resolution);
      if (HasError()) {
        // Setting the size led to an error.
        mState = JPEG_ERROR;
        return Transition::TerminateFailure();
      }

      // If we're doing a metadata decode, we're done.
      if (IsMetadataDecode()) {
        return Transition::TerminateSuccess();
      }

#ifdef RLBOX_MEASURE_TRANSITION_TIMES
      mSandbox->clear_transition_times();
#endif

      // We're doing a full decode.
      switch (mInfo.jpeg_color_space.UNSAFE_unverified()) {
        case JCS_GRAYSCALE:
        case JCS_RGB:
        case JCS_YCbCr:
          // By default, we will output directly to BGRA. If we need to apply
          // special color transforms, this may change.
          switch (SurfaceFormat::OS_RGBX) {
            case SurfaceFormat::B8G8R8X8:
              mInfo.out_color_space = JCS_EXT_BGRX;
              break;
            case SurfaceFormat::X8R8G8B8:
              mInfo.out_color_space = JCS_EXT_XRGB;
              break;
            case SurfaceFormat::R8G8B8X8:
              mInfo.out_color_space = JCS_EXT_RGBX;
              break;
            default:
              mState = JPEG_ERROR;
              return Transition::TerminateFailure();
          }
          break;
        case JCS_CMYK:
        case JCS_YCCK:
          // libjpeg can convert from YCCK to CMYK, but not to XRGB.
          mInfo.out_color_space = JCS_CMYK;
          break;
        default:
          mState = JPEG_ERROR;
          MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                  ("} (unknown colorspace (3))"));
          return Transition::TerminateFailure();
      }

      if (mCMSMode != CMSMode::Off) {
        if ((mInProfile = GetICCProfile(mInfo)) != nullptr &&
            GetCMSOutputProfile()) {
          uint32_t profileSpace = qcms_profile_get_color_space(mInProfile);

          qcms_data_type outputType = gfxPlatform::GetCMSOSRGBAType();
          Maybe<qcms_data_type> inputType;
          if (profileSpace == icSigRgbData) {
            // We can always color manage RGB profiles since it happens at the
            // end of the pipeline.
            inputType.emplace(outputType);
          } else if (profileSpace == icSigGrayData &&
                     mInfo.jpeg_color_space.UNSAFE_unverified() == JCS_GRAYSCALE) {
            // We can only color manage gray profiles if the original color
            // space is grayscale. This means we must downscale after color
            // management since the downscaler assumes BGRA.
            mInfo.out_color_space = JCS_GRAYSCALE;
            inputType.emplace(QCMS_DATA_GRAY_8);
          }

#if 0
          // We don't currently support CMYK profiles. The following
          // code dealt with lcms types. Add something like this
          // back when we gain support for CMYK.

          // Adobe Photoshop writes YCCK/CMYK files with inverted data
          if (mInfo.out_color_space == JCS_CMYK) {
            type |= FLAVOR_SH(mInfo.saw_Adobe_marker ? 1 : 0);
          }
#endif

          if (inputType) {
            // Calculate rendering intent.
            int intent = gfxPlatform::GetRenderingIntent();
            if (intent == -1) {
              intent = qcms_profile_get_rendering_intent(mInProfile);
            }

            // Create the color management transform.
            mTransform = qcms_transform_create(mInProfile, *inputType,
                                               GetCMSOutputProfile(),
                                               outputType, (qcms_intent)intent);
          }
        } else if (mCMSMode == CMSMode::All) {
          mTransform = GetCMSsRGBTransform(SurfaceFormat::OS_RGBX);
        }
      }

      // We don't want to use the pipe buffers directly because we don't want
      // any reads on non-BGRA formatted data.
      if (mInfo.out_color_space.UNSAFE_unverified() == JCS_GRAYSCALE ||
          mInfo.out_color_space.UNSAFE_unverified() == JCS_CMYK) {
        mCMSLine = new (std::nothrow) uint32_t[mInfo.image_width.UNSAFE_unverified()];
        if (!mCMSLine) {
          mState = JPEG_ERROR;
          MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                  ("} (could allocate buffer for color conversion)"));
          return Transition::TerminateFailure();
        }
      }

      // Don't allocate a giant and superfluous memory buffer
      // when not doing a progressive decode.
      mInfo.buffered_image =
          mDecodeStyle == PROGRESSIVE && sandbox_invoke(*mSandbox, jpeg_has_multiple_scans, &mInfo).UNSAFE_unverified();

      /* Used to set up image size so arrays can be allocated */
      sandbox_invoke(*mSandbox, jpeg_calc_output_dimensions, &mInfo);

      // We handle the transform outside the pipeline if we are outputting in
      // grayscale, because the pipeline wants BGRA pixels, particularly the
      // downscaling filter, so we can't handle it after downscaling as would
      // be optimal.
      qcms_transform* pipeTransform =
          mInfo.out_color_space.UNSAFE_unverified() != JCS_GRAYSCALE ? mTransform : nullptr;

      Maybe<SurfacePipe> pipe = SurfacePipeFactory::CreateReorientSurfacePipe(
          this, Size(), OutputSize(), SurfaceFormat::OS_RGBX, pipeTransform,
          GetOrientation());
      if (!pipe) {
        mState = JPEG_ERROR;
        MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                ("} (could not initialize surface pipe)"));
        return Transition::TerminateFailure();
      }

      mPipe = std::move(*pipe);

      MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
              ("        JPEGDecoderAccounting: nsJPEGDecoder::"
               "Write -- created image frame with %ux%u pixels",
               mInfo.image_width.UNSAFE_unverified(), mInfo.image_height.UNSAFE_unverified()));

      mState = JPEG_START_DECOMPRESS;
      [[fallthrough]];  // to start decompressing.
    }

    case JPEG_START_DECOMPRESS: {
      LOG_SCOPE((mozilla::LogModule*)sJPEGLog,
                "nsJPEGDecoder::Write -- entering"
                " JPEG_START_DECOMPRESS case");
      // Step 4: set parameters for decompression

      // FIXME -- Should reset dct_method and dither mode
      // for final pass of progressive JPEG

      mInfo.dct_method = JDCT_ISLOW;
      mInfo.dither_mode = JDITHER_FS;
      mInfo.do_fancy_upsampling = TRUE;
      mInfo.enable_2pass_quant = FALSE;
      mInfo.do_block_smoothing = TRUE;

      // Step 5: Start decompressor
      if (sandbox_invoke(*mSandbox, jpeg_start_decompress, &mInfo).unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION) == FALSE) {
        MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                ("} (I/O suspension after jpeg_start_decompress())"));
        return Transition::ContinueUnbuffered(
            State::JPEG_DATA);  // I/O suspension
      }

      // If this is a progressive JPEG ...
      mState = mInfo.buffered_image.UNSAFE_unverified() ? JPEG_DECOMPRESS_PROGRESSIVE
                                    : JPEG_DECOMPRESS_SEQUENTIAL;
      [[fallthrough]];  // to decompress sequential JPEG.
    }

    case JPEG_DECOMPRESS_SEQUENTIAL: {
      if (mState == JPEG_DECOMPRESS_SEQUENTIAL) {
        LOG_SCOPE((mozilla::LogModule*)sJPEGLog,
                  "nsJPEGDecoder::Write -- "
                  "JPEG_DECOMPRESS_SEQUENTIAL case");

        switch (OutputScanlines()) {
          case WriteState::NEED_MORE_DATA:
            MOZ_LOG(
                sJPEGDecoderAccountingLog, LogLevel::Debug,
                ("} (I/O suspension after OutputScanlines() - SEQUENTIAL)"));
            return Transition::ContinueUnbuffered(
                State::JPEG_DATA);  // I/O suspension
          case WriteState::FINISHED:
            NS_ASSERTION(mInfo.output_scanline.UNSAFE_unverified() == mInfo.output_height.UNSAFE_unverified(),
                         "We didn't process all of the data!");
            mState = JPEG_DONE;
            break;
          case WriteState::FAILURE:
            mState = JPEG_ERROR;
            MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                    ("} (Error in pipeline from OutputScalines())"));
            return Transition::TerminateFailure();
        }
      }
      [[fallthrough]];  // to decompress progressive JPEG.
    }

    case JPEG_DECOMPRESS_PROGRESSIVE: {
      if (mState == JPEG_DECOMPRESS_PROGRESSIVE) {
        LOG_SCOPE((mozilla::LogModule*)sJPEGLog,
                  "nsJPEGDecoder::Write -- JPEG_DECOMPRESS_PROGRESSIVE case");
        auto AllComponentsSeen = [](auto& info) {
          bool all_components_seen = true;
          if ((info.coef_bits != nullptr).unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION)) {
            for (int c = 0; c < info.num_components.unverified_safe_because("worse case infinite loop and value only used to access tainted"); ++c) {
              tainted_boolean_hint current_component_seen = info.coef_bits[c][0] != -1;
              all_components_seen &= current_component_seen.unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION);
            }
          }
          return all_components_seen;
        };
        int status;
        tainted_jpeg<int> scan_to_display_first = 0;
        bool all_components_seen;
        all_components_seen = AllComponentsSeen(mInfo);
        if (all_components_seen) {
          scan_to_display_first = mInfo.input_scan_number;
        }

        do {
          status = sandbox_invoke(*mSandbox, jpeg_consume_input, &mInfo).UNSAFE_unverified();

          if (status == JPEG_REACHED_SOS || status == JPEG_REACHED_EOI ||
              status == JPEG_SUSPENDED) {
            // record the first scan where all components are present
            all_components_seen = AllComponentsSeen(mInfo);
            if (!(scan_to_display_first.unverified_safe_because(RLBOX_JPEG_TAINTED_ASSERTION)) && all_components_seen) {
              scan_to_display_first = mInfo.input_scan_number;
            }
          }
        } while ((status != JPEG_SUSPENDED) && (status != JPEG_REACHED_EOI));

        if (!all_components_seen) {
          return Transition::ContinueUnbuffered(
              State::JPEG_DATA);  // I/O suspension
        }
        // make sure we never try to access the non-exsitent scan 0
        if (!(scan_to_display_first.unverified_safe_because(RLBOX_JPEG_TAINTED_ASSERTION))) {
          scan_to_display_first = 1;
        }
        while (mState != JPEG_DONE) {
          if (mInfo.output_scanline.UNSAFE_unverified() == 0) {
            int scan = mInfo.input_scan_number.UNSAFE_unverified();

            // if we haven't displayed anything yet (output_scan_number==0)
            // and we have enough data for a complete scan, force output
            // of the last full scan,  but only if this last scan has seen
            // DC data from all components
            if ((mInfo.output_scan_number.UNSAFE_unverified() == 0) &&
                (scan > scan_to_display_first.unverified_safe_because("value used to set scan which is checked later")) &&
                (status != JPEG_REACHED_EOI)) {
              scan--;
            }
            MOZ_ASSERT(scan > 0, "scan number to small!");
            if (!sandbox_invoke(*mSandbox, jpeg_start_output, &mInfo, scan).unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION)) {
              MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                      ("} (I/O suspension after jpeg_start_output() -"
                       " PROGRESSIVE)"));
              return Transition::ContinueUnbuffered(
                  State::JPEG_DATA);  // I/O suspension
            }
          }

          if (mInfo.output_scanline.UNSAFE_unverified() == 0xffffff) {
            mInfo.output_scanline = 0;
          }

          switch (OutputScanlines()) {
            case WriteState::NEED_MORE_DATA:
              if (mInfo.output_scanline.UNSAFE_unverified() == 0) {
                // didn't manage to read any lines - flag so we don't call
                // jpeg_start_output() multiple times for the same scan
                mInfo.output_scanline = 0xffffff;
              }
              MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                      ("} (I/O suspension after OutputScanlines() - "
                       "PROGRESSIVE)"));
              return Transition::ContinueUnbuffered(
                  State::JPEG_DATA);  // I/O suspension
            case WriteState::FINISHED:
              NS_ASSERTION((mInfo.output_scanline == mInfo.output_height).unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION),
                           "We didn't process all of the data!");

              if (!sandbox_invoke(*mSandbox, jpeg_finish_output, &mInfo).unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION)) {
                MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                        ("} (I/O suspension after jpeg_finish_output() -"
                         " PROGRESSIVE)"));
                return Transition::ContinueUnbuffered(
                    State::JPEG_DATA);  // I/O suspension
              }

              if (sandbox_invoke(*mSandbox, jpeg_input_complete, &mInfo).UNSAFE_unverified() &&
                  (mInfo.input_scan_number.UNSAFE_unverified() == mInfo.output_scan_number.UNSAFE_unverified())) {
                mState = JPEG_DONE;
              } else {
                mInfo.output_scanline = 0;
                mPipe.ResetToFirstRow();
              }
              break;
            case WriteState::FAILURE:
              mState = JPEG_ERROR;
              MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                      ("} (Error in pipeline from OutputScalines())"));
              return Transition::TerminateFailure();
          }
        }
      }
      [[fallthrough]];  // to finish decompressing.
    }

    case JPEG_DONE: {
      LOG_SCOPE((mozilla::LogModule*)sJPEGLog,
                "nsJPEGDecoder::ProcessData -- entering"
                " JPEG_DONE case");

      // Step 7: Finish decompression

      if (sandbox_invoke(*mSandbox, jpeg_finish_decompress, &mInfo).unverified_safe_because(RLBOX_JPEG_STATE_ASSERTION) == FALSE) {
        MOZ_LOG(sJPEGDecoderAccountingLog, LogLevel::Debug,
                ("} (I/O suspension after jpeg_finish_decompress() - DONE)"));
        return Transition::ContinueUnbuffered(
            State::JPEG_DATA);  // I/O suspension
      }

      // Make sure we don't feed any more data to libjpeg-turbo.
      mState = JPEG_SINK_NON_JPEG_TRAILER;

      // We're done.
      return Transition::TerminateSuccess();
    }
    case JPEG_SINK_NON_JPEG_TRAILER:
      MOZ_LOG(sJPEGLog, LogLevel::Debug,
              ("[this=%p] nsJPEGDecoder::ProcessData -- entering"
               " JPEG_SINK_NON_JPEG_TRAILER case\n",
               this));

      MOZ_ASSERT_UNREACHABLE(
          "Should stop getting data after entering state "
          "JPEG_SINK_NON_JPEG_TRAILER");

      return Transition::TerminateSuccess();

    case JPEG_ERROR:
      MOZ_ASSERT_UNREACHABLE(
          "Should stop getting data after entering state "
          "JPEG_ERROR");

      return Transition::TerminateFailure();
  }

  MOZ_ASSERT_UNREACHABLE("Escaped the JPEG decoder state machine");
  return Transition::TerminateFailure();
}  // namespace image

LexerTransition<nsJPEGDecoder::State> nsJPEGDecoder::FinishedJPEGData() {
  // Since we set up an unbuffered read for SIZE_MAX bytes, if we actually read
  // all that data something is really wrong.
  MOZ_ASSERT_UNREACHABLE("Read the entire address space?");
  return Transition::TerminateFailure();
}

EXIFData nsJPEGDecoder::ReadExifData() const {
  tainted_jpeg<jpeg_saved_marker_ptr> marker;
  auto& mInfo = *rlbox::from_opaque(p_mInfo);

  // Locate the APP1 marker, where EXIF data is stored, in the marker list.
  for (marker = mInfo.marker_list; marker != nullptr; marker = marker->next) {
    if (marker->marker.UNSAFE_unverified() == JPEG_APP0 + 1) {
      break;
    }
  }

  // If we're at the end of the list, there's no EXIF data.
  if (!marker) {
    return EXIFData();
  }

  return EXIFParser::Parse(marker->data.UNSAFE_unverified(),
                           static_cast<uint32_t>(marker->data_length.UNSAFE_unverified()),
                           gfx::IntSize(mInfo.image_width.UNSAFE_unverified(), mInfo.image_height.UNSAFE_unverified()));
}

void nsJPEGDecoder::NotifyDone() {
  PostFrameStop(Opacity::FULLY_OPAQUE);
  PostDecodeDone();
}

WriteState nsJPEGDecoder::OutputScanlines() {
  auto& mInfo = *rlbox::from_opaque(p_mInfo);
  auto result = mPipe.WritePixelBlocks<uint32_t>(
      [&](uint32_t* aPixelBlock, int32_t aBlockSize) {
        JSAMPROW sampleRow = (JSAMPROW)(mCMSLine ? mCMSLine : aPixelBlock);

        bool used_copy = false;
        auto row_size = (mInfo.output_width * mInfo.output_components).UNSAFE_unverified();
        auto output_buffer = transfer_input_bytes(sampleRow, row_size, m_output_transfer_buffer, m_output_transfer_buffer_size, used_copy);
        auto t_output_buffer = rlbox::from_opaque(output_buffer);
        *rlbox::from_opaque(m_p_output_transfer_buffer) = t_output_buffer;

        if (sandbox_invoke(*mSandbox, jpeg_read_scanlines, &mInfo, m_p_output_transfer_buffer, 1).UNSAFE_unverified() != 1) {
          return std::make_tuple(/* aWritten */ 0,
                                 Some(WriteState::NEED_MORE_DATA));
        }

        if (used_copy) {
          memcpy(sampleRow, t_output_buffer.UNSAFE_unverified(), row_size);
        }

        switch (mInfo.out_color_space.UNSAFE_unverified()) {
          default:
            // Already outputted directly to aPixelBlock as BGRA.
            MOZ_ASSERT(!mCMSLine);
            break;
          case JCS_GRAYSCALE:
            // The transform here does both color management, and converts the
            // pixels from grayscale to BGRA. This is why we do it here, instead
            // of using ColorManagementFilter in the SurfacePipe, because the
            // other filters (e.g. DownscalingFilter) require BGRA pixels.
            MOZ_ASSERT(mCMSLine);
            qcms_transform_data(mTransform, mCMSLine, aPixelBlock,
                                mInfo.output_width.UNSAFE_unverified());
            break;
          case JCS_CMYK:
            // Convert from CMYK to BGRA
            MOZ_ASSERT(mCMSLine);
            cmyk_convert_bgra(mCMSLine, aPixelBlock, aBlockSize);
            break;
        }

        return std::make_tuple(aBlockSize, Maybe<WriteState>());
      });

  Maybe<SurfaceInvalidRect> invalidRect = mPipe.TakeInvalidRect();
  if (invalidRect) {
    PostInvalidation(invalidRect->mInputSpaceRect,
                     Some(invalidRect->mOutputSpaceRect));
  }

  return result;
}

// Override the standard error method in the IJG JPEG decoder code.
METHODDEF(void)
my_error_exit(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_common_ptr> cinfo) {
  nsJPEGDecoder* decoder = (nsJPEGDecoder*) jpegRendererSaved;
  tainted_jpeg<decoder_error_mgr*> err = rlbox::sandbox_reinterpret_cast<decoder_error_mgr*>(cinfo->err);

  // Convert error to a browser error code
  nsresult error_code;
  switch (err->pub.msg_code.unverified_safe_because("Only checking to set an error code")) {
    case JERR_OUT_OF_MEMORY:
      error_code = NS_ERROR_OUT_OF_MEMORY;
      break;
    case JERR_UNKNOWN_MARKER:
      error_code = NS_ERROR_ILLEGAL_VALUE;
      break;
    case JERR_SOF_UNSUPPORTED:
      error_code = NS_ERROR_INVALID_CONTENT_ENCODING;
      break;
    default:
      error_code = NS_ERROR_FAILURE;
  }

#ifdef DEBUG
  // char buffer[JMSG_LENGTH_MAX];

  // Create the message
  //(*err->pub.format_message)(cinfo, buffer);

  fprintf(stderr, "JPEG decoding error");
#endif

  // Return control to the setjmp point.  We pass an nsresult masquerading as
  // an int, which works because the setjmp() caller casts it back.
  if (!decoder->m_jmpBuffValid) {
    abort();
  } else {
    decoder->m_jmpBuffValid = false;
  }
  longjmp(decoder->m_jmpBuff, static_cast<int>(error_code));
}

static void progress_monitor(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_common_ptr> info) {
  tainted_jpeg<int> scan = rlbox::sandbox_reinterpret_cast<j_decompress_ptr>(info)->input_scan_number;
  // Progressive images with a very large number of scans can cause the decoder
  // to hang. Here we use the progress monitor to abort on a very large number
  // of scans. 1000 is arbitrary, but much larger than the number of scans we
  // might expect in a normal image.
  if (scan.unverified_safe_because("Early error exit") >= 1000) {
    my_error_exit(aSandbox, info);
  }
}

/*******************************************************************************
 * This is the callback routine from the IJG JPEG library used to supply new
 * data to the decompressor when its input buffer is exhausted.  It juggles
 * multiple buffers in an attempt to avoid unnecessary copying of input data.
 *
 * (A simpler scheme is possible: It's much easier to use only a single
 * buffer; when fill_input_buffer() is called, move any unconsumed data
 * (beyond the current pointer/count) down to the beginning of this buffer and
 * then load new data into the remaining buffer space.  This approach requires
 * a little more data copying but is far easier to get right.)
 *
 * At any one time, the JPEG decompressor is either reading from the necko
 * input buffer, which is volatile across top-level calls to the IJG library,
 * or the "backtrack" buffer.  The backtrack buffer contains the remaining
 * unconsumed data from the necko buffer after parsing was suspended due
 * to insufficient data in some previous call to the IJG library.
 *
 * When suspending, the decompressor will back up to a convenient restart
 * point (typically the start of the current MCU). The variables
 * next_input_byte & bytes_in_buffer indicate where the restart point will be
 * if the current call returns FALSE.  Data beyond this point must be
 * rescanned after resumption, so it must be preserved in case the decompressor
 * decides to backtrack.
 *
 * Returns:
 *  TRUE if additional data is available, FALSE if no data present and
 *   the JPEG library should therefore suspend processing of input stream
 ******************************************************************************/

/******************************************************************************/
/* data source manager method                                                 */
/******************************************************************************/

/******************************************************************************/
/* data source manager method
        Initialize source.  This is called by jpeg_read_header() before any
        data is actually read.  May leave
        bytes_in_buffer set to 0 (in which case a fill_input_buffer() call
        will occur immediately).
*/
METHODDEF(void)
init_source(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd) {}


/******************************************************************************/
/* data source manager method
        Skip num_bytes worth of data.  The buffer pointer and count should
        be advanced over num_bytes input bytes, refilling the buffer as
        needed.  This is used to skip over a potentially large amount of
        uninteresting data (such as an APPn marker).  In some applications
        it may be possible to optimize away the reading of the skipped data,
        but it's not clear that being smart is worth much trouble; large
        skips are uncommon.  bytes_in_buffer may be zero on return.
        A zero or negative skip count should be treated as a no-op.
*/
METHODDEF(void)
skip_input_data(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd, tainted_jpeg<long> num_bytes) {
  tainted_jpeg<jpeg_source_mgr*> src = jd->src;
  nsJPEGDecoder* decoder = (nsJPEGDecoder*) jpegRendererSaved;

  if ((num_bytes > rlbox::sandbox_static_cast<long>(src->bytes_in_buffer)).unverified_safe_because(
    "Branches either set tainted data or mBytesToSkip which is checked")) {
    // Can't skip it all right now until we get more data from
    // network stream. Set things up so that fill_input_buffer
    // will skip remaining amount.
    decoder->mBytesToSkip = (rlbox::sandbox_static_cast<size_t>(num_bytes) - src->bytes_in_buffer).UNSAFE_unverified();
    src->next_input_byte += src->bytes_in_buffer;
    src->bytes_in_buffer = 0;

  } else {
    // Simple case. Just advance buffer pointer

    src->bytes_in_buffer -= rlbox::sandbox_static_cast<size_t>(num_bytes);
    src->next_input_byte += num_bytes;
  }
}

tainted_opaque_jpeg<unsigned char*> nsJPEGDecoder::transfer_input_bytes(
  unsigned char* buffer, size_t size,
  tainted_opaque_jpeg<unsigned char*>& transfer_buffer,
  size_t& transfer_buffer_size,
  bool& used_copy)
{
  if (size == 0) {
    if(buffer == nullptr) {
      tainted_opaque_jpeg<unsigned char*> ret;
      ret.set_zero();
      return ret;
    } else if (rlbox::from_opaque(transfer_buffer) != nullptr) {
      return transfer_buffer;
    } else {
      size = 1;
    }
  }

  if (transfer_buffer_size >= size) {
    used_copy = true;
    return transfer_buffer;
  } else if (transfer_buffer_size != 0) {
    mSandbox->free_in_sandbox(transfer_buffer);
    transfer_buffer_size = 0;
  }

  const bool free_src_on_copy = false;
  auto transferred = rlbox::copy_memory_or_grant_access(*mSandbox, buffer, size, free_src_on_copy, used_copy);
  MOZ_RELEASE_ASSERT(transferred != nullptr);

  if (used_copy) {
    transfer_buffer = transferred.to_opaque();
    transfer_buffer_size = size;
    return transfer_buffer;
  } else {
    return transferred.to_opaque();
  }
}

tainted_opaque_jpeg<unsigned char*> nsJPEGDecoder::transfer_input_bytes(
  unsigned char* buffer, size_t size,
  tainted_opaque_jpeg<unsigned char*>& transfer_buffer,
  size_t& transfer_buffer_size)
{

  bool used_copy = false;
  return transfer_input_bytes(buffer, size, transfer_buffer, transfer_buffer_size, used_copy);
}

/******************************************************************************/
/* data source manager method
        This is called whenever bytes_in_buffer has reached zero and more
        data is wanted.  In typical applications, it should read fresh data
        into the buffer (ignoring the current state of next_input_byte and
        bytes_in_buffer), reset the pointer & count to the start of the
        buffer, and return TRUE indicating that the buffer has been reloaded.
        It is not necessary to fill the buffer entirely, only to obtain at
        least one more byte.  bytes_in_buffer MUST be set to a positive value
        if TRUE is returned.  A FALSE return should only be used when I/O
        suspension is desired.
*/
METHODDEF(tainted_jpeg<boolean>)
fill_input_buffer(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd) {
  tainted_jpeg<jpeg_source_mgr*> src = jd->src;
  nsJPEGDecoder* decoder = (nsJPEGDecoder*) jpegRendererSaved;

  if (decoder->mReading) {
    const JOCTET* new_buffer = decoder->mSegment;
    uint32_t new_buflen = decoder->mSegmentLen;

    if (!new_buffer || new_buflen == 0) {
      return false;  // suspend
    }

    decoder->mSegmentLen = 0;

    if (decoder->mBytesToSkip) {
      if (decoder->mBytesToSkip < new_buflen) {
        // All done skipping bytes; Return what's left.
        new_buffer += decoder->mBytesToSkip;
        new_buflen -= decoder->mBytesToSkip;
        decoder->mBytesToSkip = 0;
      } else {
        // Still need to skip some more data in the future
        decoder->mBytesToSkip -= (size_t)new_buflen;
        return false;  // suspend
      }
    }

    decoder->mBackBufferUnreadLen = src->bytes_in_buffer.UNSAFE_unverified();

    auto transferred = decoder->transfer_input_bytes(const_cast<JOCTET *>(new_buffer), new_buflen,
      decoder->m_input_transfer_buffer, decoder->m_input_transfer_buffer_size);
    src->next_input_byte = rlbox::from_opaque(transferred);
    src->bytes_in_buffer = (size_t)new_buflen;
    decoder->mReading = false;

    return true;
  }

  if (src->next_input_byte.UNSAFE_unverified() != decoder->mSegment) {
    // Backtrack data has been permanently consumed.
    decoder->mBackBufferUnreadLen = 0;
    decoder->mBackBufferLen = 0;
  }

  // Save remainder of netlib buffer in backtrack buffer
  const uint32_t new_backtrack_buflen =
      src->bytes_in_buffer.UNSAFE_unverified() + decoder->mBackBufferLen;

  // Make sure backtrack buffer is big enough to hold new data.
  if (decoder->mBackBufferSize < new_backtrack_buflen) {
    // Check for malformed MARKER segment lengths, before allocating space
    // for it
    auto& mInfo = *rlbox::from_opaque(decoder->p_mInfo);
    if (new_backtrack_buflen > MAX_JPEG_MARKER_LENGTH) {
      my_error_exit(*(decoder->mSandbox), rlbox::sandbox_reinterpret_cast<j_common_ptr>(&mInfo));
    }

    // Round up to multiple of 256 bytes.
    const size_t roundup_buflen = ((new_backtrack_buflen + 255) >> 8) << 8;
    JOCTET* buf = (JOCTET*)realloc(decoder->mBackBuffer, roundup_buflen);
    // Check for OOM
    if (!buf) {
      mInfo.err->msg_code = (int) JERR_OUT_OF_MEMORY;
      my_error_exit(*(decoder->mSandbox), rlbox::sandbox_reinterpret_cast<j_common_ptr>(&mInfo));
    }
    decoder->mBackBuffer = buf;
    decoder->mBackBufferSize = roundup_buflen;
  }

  // Ensure we actually have a backtrack buffer. Without it, then we know that
  // there is no data to copy and bytes_in_buffer is already zero.
  if (decoder->mBackBuffer) {
    // Copy remainder of netlib segment into backtrack buffer.
    memmove(decoder->mBackBuffer + decoder->mBackBufferLen,
            src->next_input_byte.UNSAFE_unverified(), src->bytes_in_buffer.UNSAFE_unverified());
  } else {
    MOZ_ASSERT(src->bytes_in_buffer.UNSAFE_unverified() == 0);
    MOZ_ASSERT(decoder->mBackBufferLen == 0);
    MOZ_ASSERT(decoder->mBackBufferUnreadLen == 0);
  }

  // Point to start of data to be rescanned.
  auto target_ptr = decoder->mBackBuffer + decoder->mBackBufferLen - decoder->mBackBufferUnreadLen;
  auto transferred = decoder->transfer_input_bytes(const_cast<JOCTET *>(target_ptr), decoder->mBackBufferUnreadLen,
    decoder->m_input_transfer_buffer, decoder->m_input_transfer_buffer_size);
  src->next_input_byte = rlbox::from_opaque(transferred);
  src->bytes_in_buffer += decoder->mBackBufferUnreadLen;
  decoder->mBackBufferLen = (size_t)new_backtrack_buflen;
  decoder->mReading = true;

  return false;
}

/******************************************************************************/
/* data source manager method */
/*
 * Terminate source --- called by jpeg_finish_decompress() after all
 * data has been read to clean up JPEG source manager. NOT called by
 * jpeg_abort() or jpeg_destroy().
 */
METHODDEF(void)
term_source(rlbox_sandbox_jpeg& aSandbox, tainted_jpeg<j_decompress_ptr> jd) {
  nsJPEGDecoder* decoder = (nsJPEGDecoder*) jpegRendererSaved;

  // This function shouldn't be called if we ran into an error we didn't
  // recover from.
  MOZ_ASSERT(decoder->mState != JPEG_ERROR,
             "Calling term_source on a JPEG with mState == JPEG_ERROR!");

  // Notify using a helper method to get around protectedness issues.
  decoder->NotifyDone();
}

}  // namespace image
}  // namespace mozilla

///*************** Inverted CMYK -> RGB conversion *************************
/// Input is (Inverted) CMYK stored as 4 bytes per pixel.
/// Output is RGB stored as 3 bytes per pixel.
/// @param aInput Points to row buffer containing the CMYK bytes for each pixel
///               in the row.
/// @param aOutput Points to row buffer to write BGRA to.
/// @param aWidth Number of pixels in the row.
static void cmyk_convert_bgra(uint32_t* aInput, uint32_t* aOutput,
                              int32_t aWidth) {
  uint8_t* input = reinterpret_cast<uint8_t*>(aInput);

  for (int32_t i = 0; i < aWidth; ++i) {
    // Source is 'Inverted CMYK', output is RGB.
    // See: http://www.easyrgb.com/math.php?MATH=M12#text12
    // Or:  http://www.ilkeratalay.com/colorspacesfaq.php#rgb

    // From CMYK to CMY
    // C = ( C * ( 1 - K ) + K )
    // M = ( M * ( 1 - K ) + K )
    // Y = ( Y * ( 1 - K ) + K )

    // From Inverted CMYK to CMY is thus:
    // C = ( (1-iC) * (1 - (1-iK)) + (1-iK) ) => 1 - iC*iK
    // Same for M and Y

    // Convert from CMY (0..1) to RGB (0..1)
    // R = 1 - C => 1 - (1 - iC*iK) => iC*iK
    // G = 1 - M => 1 - (1 - iM*iK) => iM*iK
    // B = 1 - Y => 1 - (1 - iY*iK) => iY*iK

    // Convert from Inverted CMYK (0..255) to RGB (0..255)
    const uint32_t iC = input[0];
    const uint32_t iM = input[1];
    const uint32_t iY = input[2];
    const uint32_t iK = input[3];

    const uint8_t r = iC * iK / 255;
    const uint8_t g = iM * iK / 255;
    const uint8_t b = iY * iK / 255;

    *aOutput++ = (0xFF << mozilla::gfx::SurfaceFormatBit::OS_A) |
                 (r << mozilla::gfx::SurfaceFormatBit::OS_R) |
                 (g << mozilla::gfx::SurfaceFormatBit::OS_G) |
                 (b << mozilla::gfx::SurfaceFormatBit::OS_B);
    input += 4;
  }
}
