// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/encode_video.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#ifdef __linux__
#include <libavutil/hwcontext_drm.h>
#ifdef RUSTDESK_HAS_AVFILTER
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
#endif
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <unistd.h>
#include <fstream>
#include <vector>
#endif

#include "common.h"

#define LOG_MODULE "FFMPEG_RAM_ENC"
#include <log.h>
#include <util.h>
#ifdef _WIN32
#include "win.h"
#endif

#define DRM_FOURCC_CODE(a, b, c, d)                                            \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |              \
   ((uint32_t)(d) << 24))

static int calculate_offset_length(int pix_fmt, int height, const int *linesize,
                                   int *offset, int *length) {
  switch (pix_fmt) {
  case AV_PIX_FMT_YUV420P:
    offset[0] = linesize[0] * height;
    offset[1] = offset[0] + linesize[1] * height / 2;
    *length = offset[1] + linesize[2] * height / 2;
    break;
  case AV_PIX_FMT_NV12:
    offset[0] = linesize[0] * height;
    *length = offset[0] + linesize[1] * height / 2;
    break;
  default:
    LOG_ERROR(std::string("unsupported pixfmt") + std::to_string(pix_fmt));
    return -1;
  }

  return 0;
}

extern "C" int ffmpeg_ram_get_linesize_offset_length(int pix_fmt, int width,
                                                     int height, int align,
                                                     int *linesize, int *offset,
                                                     int *length) {
  AVFrame *frame = NULL;
  int ioffset[AV_NUM_DATA_POINTERS] = {0};
  int ilength = 0;
  int ret = -1;

  if (!(frame = av_frame_alloc())) {
    LOG_ERROR(std::string("Alloc frame failed"));
    goto _exit;
  }

  frame->format = pix_fmt;
  frame->width = width;
  frame->height = height;

  if ((ret = av_frame_get_buffer(frame, align)) < 0) {
    LOG_ERROR(std::string("av_frame_get_buffer, ret = ") + av_err2str(ret));
    goto _exit;
  }
  if (linesize) {
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
      linesize[i] = frame->linesize[i];
  }
  if (offset || length) {
    ret = calculate_offset_length(pix_fmt, height, frame->linesize, ioffset,
                                  &ilength);
    if (ret < 0)
      goto _exit;
  }
  if (offset) {
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      if (ioffset[i] == 0)
        break;
      offset[i] = ioffset[i];
    }
  }
  if (length)
    *length = ilength;

  ret = 0;
_exit:
  if (frame)
    av_frame_free(&frame);
  return ret;
}

namespace {
typedef void (*RamEncodeCallback)(const uint8_t *data, int len, int64_t pts,
                                  int key, const void *obj);

typedef struct FfmpegDmabufPlane {
  int fd;
  uint32_t stride;
  uint32_t offset;
} FfmpegDmabufPlane;

typedef struct FfmpegDmabufFrame {
  int width;
  int height;
  int encode_width;
  int encode_height;
  uint32_t fourcc;
  uint64_t modifier;
  int nb_planes;
  FfmpegDmabufPlane planes[AV_NUM_DATA_POINTERS];
} FfmpegDmabufFrame;

#ifdef __linux__
static void free_drm_prime_descriptor(void *, uint8_t *data) {
  AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
  if (!desc)
    return;
  for (int i = 0; i < desc->nb_objects; ++i) {
    if (desc->objects[i].fd >= 0)
      close(desc->objects[i].fd);
  }
  av_free(desc);
}

static uint32_t drm_plane_height(uint32_t fourcc, int frame_height,
                                 int plane_index) {
  uint32_t height = frame_height > 0 ? static_cast<uint32_t>(frame_height) : 0;
  switch (fourcc) {
  case DRM_FOURCC_CODE('N', 'V', '1', '2'):
    return plane_index == 0 ? height : (height + 1) / 2;
  default:
    return height;
  }
}
#endif

class FFmpegRamEncoder {
public:
  AVCodecContext *c_ = NULL;
  AVFrame *frame_ = NULL;
  AVPacket *pkt_ = NULL;
  std::string name_;
  std::string mc_name_; // for mediacodec

  int width_ = 0;
  int height_ = 0;
  AVPixelFormat pixfmt_ = AV_PIX_FMT_NV12;
  int align_ = 0;
  int rc_ = 0;
  int quality_ = 0;
  int kbs_ = 0;
  int q_ = 0;
  int fps_ = 30;
  int gop_ = 0xFFFF;
  int thread_count_ = 1;
  int gpu_ = 0;
  RamEncodeCallback callback_ = NULL;
  int offset_[AV_NUM_DATA_POINTERS] = {0};

  AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pixfmt_ = AV_PIX_FMT_NONE;
  AVBufferRef *hw_device_ctx_ = NULL;
#ifdef __linux__
  AVBufferRef *drm_device_ctx_ = NULL;
#endif
  AVFrame *hw_frame_ = NULL;

  FFmpegRamEncoder(const char *name, const char *mc_name, int width, int height,
                   int pixfmt, int align, int fps, int gop, int rc, int quality,
                   int kbs, int q, int thread_count, int gpu,
                   RamEncodeCallback callback) {
    name_ = name;
    mc_name_ = mc_name ? mc_name : "";
    width_ = width;
    height_ = height;
    pixfmt_ = (AVPixelFormat)pixfmt;
    align_ = align;
    fps_ = fps;
    gop_ = gop;
    rc_ = rc;
    quality_ = quality;
    kbs_ = kbs;
    q_ = q;
    thread_count_ = thread_count;
    gpu_ = gpu;
    callback_ = callback;
    if (name_.find("vaapi") != std::string::npos) {
      hw_device_type_ = AV_HWDEVICE_TYPE_VAAPI;
      hw_pixfmt_ = AV_PIX_FMT_VAAPI;
    } else if (name_.find("nvenc") != std::string::npos) {
#ifdef _WIN32
      hw_device_type_ = AV_HWDEVICE_TYPE_D3D11VA;
      hw_pixfmt_ = AV_PIX_FMT_D3D11;
#endif
    }
  }

  ~FFmpegRamEncoder() {}

  bool init(int *linesize, int *offset, int *length) {
    const AVCodec *codec = NULL;

    int ret;

    if (!(codec = avcodec_find_encoder_by_name(name_.c_str()))) {
      LOG_ERROR(std::string("Codec ") + name_ + " not found");
      return false;
    }

    if (!(c_ = avcodec_alloc_context3(codec))) {
      LOG_ERROR(std::string("Could not allocate video codec context"));
      return false;
    }

    if (hw_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      std::string device = "";
#ifdef _WIN32
      if (name_.find("nvenc") != std::string::npos) {
        int index = Adapters::GetFirstAdapterIndex(
            AdapterVendor::ADAPTER_VENDOR_NVIDIA);
        if (index >= 0) {
          device = std::to_string(index);
        }
      }
#endif
#ifdef __linux__
      if (hw_device_type_ == AV_HWDEVICE_TYPE_VAAPI) {
        // For dmabuf import we need a DRM device to wrap the incoming PRIME fd.
        // FFmpeg can derive VAAPI *from* DRM (vaapi_device_derive) but NOT a DRM
        // device from VAAPI -- hwcontext_drm has no device_derive, so that path
        // returns ENOSYS ("Function not implemented"). So open a render node
        // explicitly and derive the VAAPI device from it. (Passing NULL to
        // av_hwdevice_ctx_create(DRM) is also wrong: ffmpeg does open(NULL) ->
        // EFAULT "Bad address".)
        //
        // Multi-GPU: on Intel-iGPU + NVIDIA-dGPU laptops renderD128 is often the
        // NVIDIA node (nouveau), which exposes VAAPI but has NO encode entrypoints,
        // so the encoder silently fails and the caller falls back to software. So
        // honor RUSTDESK_VAAPI_RENDER_NODE, else scan /dev/dri/renderD* and skip
        // nvidia/nouveau, picking the first node we can create a VAAPI device on
        // (i915/amdgpu). Done here (not via env) because the rustdesk server is a
        // separate process that would not inherit an env override.
        std::vector<std::string> candidates;
        const char *forced = getenv("RUSTDESK_VAAPI_RENDER_NODE");
        if (forced && *forced) {
          candidates.emplace_back(forced);
        } else {
          for (int i = 128; i < 136; ++i) {
            std::string idx = std::to_string(i);
            std::ifstream uevent("/sys/class/drm/renderD" + idx +
                                 "/device/uevent");
            if (!uevent) {
              continue;
            }
            std::string line, driver;
            while (std::getline(uevent, line)) {
              if (line.rfind("DRIVER=", 0) == 0) {
                driver = line.substr(7);
                break;
              }
            }
            if (driver.empty() || driver == "nvidia" || driver == "nouveau") {
              continue;  // no VAAPI encode on these
            }
            candidates.emplace_back("/dev/dri/renderD" + idx);
          }
          if (candidates.empty()) {
            candidates.emplace_back("/dev/dri/renderD128");
          }
        }

        ret = -1;
        for (const std::string &node : candidates) {
          ret = av_hwdevice_ctx_create(&drm_device_ctx_, AV_HWDEVICE_TYPE_DRM,
                                       node.c_str(), NULL, 0);
          if (ret < 0) {
            continue;
          }
          ret = av_hwdevice_ctx_create_derived(
              &hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI, drm_device_ctx_, 0);
          if (ret >= 0) {
            LOG_INFO(std::string("VAAPI encode render node: ") + node);
            break;
          }
          av_buffer_unref(&drm_device_ctx_);
        }
        if (ret < 0) {
          LOG_ERROR(std::string("failed to create a VAAPI device from any "
                                "render node, ret = ") +
                    av_err2str(ret));
          return false;
        }
      } else
#endif
      {
        ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_device_type_,
                                     device.length() == 0 ? NULL : device.c_str(),
                                     NULL, 0);
        if (ret < 0) {
          LOG_ERROR(std::string("av_hwdevice_ctx_create failed"));
          return false;
        }
      }
      if (set_hwframe_ctx() != 0) {
        LOG_ERROR(std::string("set_hwframe_ctx failed"));
        return false;
      }
      hw_frame_ = av_frame_alloc();
      if (!hw_frame_) {
        LOG_ERROR(std::string("av_frame_alloc failed"));
        return false;
      }
      if ((ret = av_hwframe_get_buffer(c_->hw_frames_ctx, hw_frame_, 0)) < 0) {
        LOG_ERROR(std::string("av_hwframe_get_buffer failed, ret = ") + av_err2str(ret));
        return false;
      }
      if (!hw_frame_->hw_frames_ctx) {
        LOG_ERROR(std::string("hw_frame_->hw_frames_ctx is NULL"));
        return false;
      }
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR(std::string("Could not allocate video frame"));
      return false;
    }
    frame_->format = pixfmt_;
    frame_->width = width_;
    frame_->height = height_;

    if ((ret = av_frame_get_buffer(frame_, align_)) < 0) {
      LOG_ERROR(std::string("av_frame_get_buffer failed, ret = ") + av_err2str(ret));
      return false;
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR(std::string("Could not allocate video packet"));
      return false;
    }

    /* resolution must be a multiple of two */
    c_->width = width_;
    c_->height = height_;
    c_->pix_fmt =
        hw_pixfmt_ != AV_PIX_FMT_NONE ? hw_pixfmt_ : (AVPixelFormat)pixfmt_;
    c_->sw_pix_fmt = (AVPixelFormat)pixfmt_;
    util_encode::set_av_codec_ctx(c_, name_, kbs_, gop_, fps_);
    if (!util_encode::set_lantency_free(c_->priv_data, name_)) {
      LOG_ERROR(std::string("set_lantency_free failed, name: ") + name_);
      return false;
    }
    // util_encode::set_quality(c_->priv_data, name_, quality_);
    util_encode::set_rate_control(c_, name_, rc_, q_);
    util_encode::set_gpu(c_->priv_data, name_, gpu_);
    util_encode::force_hw(c_->priv_data, name_);
    util_encode::set_others(c_->priv_data, name_);
    if (name_.find("mediacodec") != std::string::npos) {
      if (mc_name_.length() > 0) {
        LOG_INFO(std::string("mediacodec codec_name: ") + mc_name_);
        if ((ret = av_opt_set(c_->priv_data, "codec_name", mc_name_.c_str(),
                              0)) < 0) {
          LOG_ERROR(std::string("mediacodec codec_name failed, ret = ") + av_err2str(ret));
        }
      }
    }

    if ((ret = avcodec_open2(c_, codec, NULL)) < 0) {
      LOG_ERROR(std::string("avcodec_open2 failed, ret = ") + av_err2str(ret) +
                ", name: " + name_);
      return false;
    }

    if (ffmpeg_ram_get_linesize_offset_length(pixfmt_, width_, height_, align_,
                                              NULL, offset_, length) != 0)
      return false;

    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      linesize[i] = frame_->linesize[i];
      offset[i] = offset_[i];
    }
    return true;
  }

  int encode(const uint8_t *data, int length, const void *obj, uint64_t ms) {
    int ret;

    if ((ret = av_frame_make_writable(frame_)) != 0) {
      LOG_ERROR(std::string("av_frame_make_writable failed, ret = ") + av_err2str(ret));
      return ret;
    }
    if ((ret = fill_frame(frame_, (uint8_t *)data, length, offset_)) != 0)
      return ret;
    AVFrame *tmp_frame;
    if (hw_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      if ((ret = av_hwframe_transfer_data(hw_frame_, frame_, 0)) < 0) {
        LOG_ERROR(std::string("av_hwframe_transfer_data failed, ret = ") + av_err2str(ret));
        return ret;
      }
      tmp_frame = hw_frame_;
    } else {
      tmp_frame = frame_;
    }

    return do_encode(tmp_frame, obj, ms);
  }

#ifdef __linux__
  int encode_dmabuf(const FfmpegDmabufFrame *dmabuf, const void *obj,
                    uint64_t ms) {
    if (!dmabuf) {
      LOG_ERROR(std::string("encode_dmabuf: frame is NULL"));
      return -1;
    }
    if (hw_device_type_ == AV_HWDEVICE_TYPE_NONE || hw_pixfmt_ == AV_PIX_FMT_NONE ||
        !c_ || !c_->hw_frames_ctx) {
      LOG_ERROR(std::string("encode_dmabuf: hardware encoder is unavailable"));
      return -1;
    }
    if (dmabuf->encode_width != width_ || dmabuf->encode_height != height_ ||
        dmabuf->width < width_ || dmabuf->height < height_) {
      LOG_ERROR(std::string("encode_dmabuf: frame size mismatch, got capture ") +
                std::to_string(dmabuf->width) + "x" +
                std::to_string(dmabuf->height) + ", encode " +
                std::to_string(dmabuf->encode_width) + "x" +
                std::to_string(dmabuf->encode_height) + ", expected encode " +
                std::to_string(width_) + "x" + std::to_string(height_));
      return -1;
    }

    AVFrame *drm_frame = create_drm_prime_frame(dmabuf);
    if (!drm_frame)
      return -1;

    AVPixelFormat drm_sw_format = drm_fourcc_to_av_pix_fmt(dmabuf->fourcc);
    AVFrame *mapped_frame = av_frame_alloc();
    if (!mapped_frame) {
      av_frame_free(&drm_frame);
      LOG_ERROR(std::string("encode_dmabuf: av_frame_alloc failed"));
      return -1;
    }

    AVBufferRef *source_hw_frames_ctx = NULL;
    AVBufferRef *map_hw_frames_ctx = c_->hw_frames_ctx;
#ifdef RUSTDESK_HAS_AVFILTER
    if (drm_sw_format != pixfmt_) {
      source_hw_frames_ctx = create_vaapi_hw_frames_ctx(drm_sw_format);
      if (!source_hw_frames_ctx) {
        av_frame_free(&mapped_frame);
        av_frame_free(&drm_frame);
        return -1;
      }
      map_hw_frames_ctx = source_hw_frames_ctx;
    }
#endif
    int ret = map_dmabuf_to_hw_frame(drm_frame, mapped_frame,
                                     map_hw_frames_ctx,
                                     AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (ret < 0) {
      av_frame_unref(mapped_frame);
      ret = map_dmabuf_to_hw_frame(drm_frame, mapped_frame, map_hw_frames_ctx,
                                   AV_HWFRAME_MAP_READ);
    }
    if (ret < 0) {
      LOG_ERROR(std::string("encode_dmabuf: av_hwframe_map failed, ret = ") +
                av_err2str(ret));
      if (source_hw_frames_ctx)
        av_buffer_unref(&source_hw_frames_ctx);
      av_frame_free(&mapped_frame);
      av_frame_free(&drm_frame);
      return ret;
    }

#ifdef RUSTDESK_HAS_AVFILTER
    AVFrame *encode_frame = mapped_frame;
    AVFrame *filtered_frame = NULL;
    if (drm_sw_format != pixfmt_) {
      filtered_frame = av_frame_alloc();
      if (!filtered_frame) {
        LOG_ERROR(std::string("encode_dmabuf: filtered av_frame_alloc failed"));
        if (source_hw_frames_ctx)
          av_buffer_unref(&source_hw_frames_ctx);
        av_frame_free(&mapped_frame);
        av_frame_free(&drm_frame);
        return -1;
      }
      ret = filter_vaapi_to_nv12(mapped_frame, filtered_frame);
      if (ret < 0) {
        av_frame_free(&filtered_frame);
        if (source_hw_frames_ctx)
          av_buffer_unref(&source_hw_frames_ctx);
        av_frame_free(&mapped_frame);
        av_frame_free(&drm_frame);
        return ret;
      }
      encode_frame = filtered_frame;
    }

    ret = do_encode(encode_frame, obj, ms);
    if (filtered_frame)
      av_frame_free(&filtered_frame);
#else
    if (drm_sw_format != pixfmt_) {
      LOG_ERROR(std::string("encode_dmabuf: DRM fourcc requires VAAPI format conversion, but libavfilter is unavailable"));
      av_frame_free(&mapped_frame);
      av_frame_free(&drm_frame);
      return -1;
    }
    ret = do_encode(mapped_frame, obj, ms);
#endif
    if (source_hw_frames_ctx)
      av_buffer_unref(&source_hw_frames_ctx);
    av_frame_free(&mapped_frame);
    av_frame_free(&drm_frame);
    return ret;
  }
#else
  int encode_dmabuf(const FfmpegDmabufFrame *, const void *, uint64_t) {
    LOG_ERROR(std::string("encode_dmabuf: DRM PRIME input is unsupported on this platform"));
    return -1;
  }
#endif

  void free_encoder() {
    if (pkt_)
      av_packet_free(&pkt_);
    if (frame_)
      av_frame_free(&frame_);
    if (hw_frame_)
      av_frame_free(&hw_frame_);
    if (hw_device_ctx_)
      av_buffer_unref(&hw_device_ctx_);
#ifdef __linux__
    if (drm_device_ctx_)
      av_buffer_unref(&drm_device_ctx_);
#endif
    if (c_)
      avcodec_free_context(&c_);
  }

  int set_bitrate(int kbs) {
    return util_encode::change_bit_rate(c_, name_, kbs) ? 0 : -1;
  }

private:
  int set_hwframe_ctx() {
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx_))) {
      LOG_ERROR(std::string("av_hwframe_ctx_alloc failed"));
      return -1;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = hw_pixfmt_;
    frames_ctx->sw_format = (AVPixelFormat)pixfmt_;
    frames_ctx->width = width_;
    frames_ctx->height = height_;
    frames_ctx->initial_pool_size = 1;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
      av_buffer_unref(&hw_frames_ref);
      return err;
    }
    c_->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!c_->hw_frames_ctx) {
      LOG_ERROR(std::string("av_buffer_ref failed"));
      err = -1;
    }
    av_buffer_unref(&hw_frames_ref);
    return err;
  }

  int do_encode(AVFrame *frame, const void *obj, int64_t ms) {
    int ret;
    bool encoded = false;
    frame->pts = ms;
    if ((ret = avcodec_send_frame(c_, frame)) < 0) {
      LOG_ERROR(std::string("avcodec_send_frame failed, ret = ") + av_err2str(ret));
      return ret;
    }

    auto start = util::now();
    while (ret >= 0 && util::elapsed_ms(start) < DECODE_TIMEOUT_MS) {
      if ((ret = avcodec_receive_packet(c_, pkt_)) < 0) {
        if (ret != AVERROR(EAGAIN)) {
          LOG_ERROR(std::string("avcodec_receive_packet failed, ret = ") + av_err2str(ret));
        }
        goto _exit;
      }
      if (!pkt_->data || !pkt_->size) {
        LOG_ERROR(std::string("avcodec_receive_packet failed, pkt size is 0"));
        goto _exit;
      }
      encoded = true;
      callback_(pkt_->data, pkt_->size, pkt_->pts,
                pkt_->flags & AV_PKT_FLAG_KEY, obj);
    }
  _exit:
    av_packet_unref(pkt_);
    if (encoded)
      return 0;
    // No packet was produced. EAGAIN only means the encoder needs more input
    // (warm-up / lookahead delay), which is not an error; report success so the
    // caller keeps feeding frames instead of treating it as an encode failure.
    if (ret == AVERROR(EAGAIN))
      return 0;
    return -1;
  }

#ifdef __linux__
  AVFrame *create_drm_prime_frame(const FfmpegDmabufFrame *dmabuf) {
    if (dmabuf->nb_planes <= 0 || dmabuf->nb_planes > AV_DRM_MAX_PLANES) {
      LOG_ERROR(std::string("create_drm_prime_frame: unsupported plane count ") +
                std::to_string(dmabuf->nb_planes));
      return NULL;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
      LOG_ERROR(std::string("create_drm_prime_frame: av_frame_alloc failed"));
      return NULL;
    }

    AVDRMFrameDescriptor *desc =
        (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
      av_frame_free(&frame);
      LOG_ERROR(std::string("create_drm_prime_frame: av_mallocz failed"));
      return NULL;
    }
    for (int i = 0; i < AV_DRM_MAX_PLANES; ++i)
      desc->objects[i].fd = -1;

    for (int i = 0; i < dmabuf->nb_planes; ++i) {
      int fd = dup(dmabuf->planes[i].fd);
      if (fd < 0) {
        LOG_ERROR(std::string("create_drm_prime_frame: dup failed"));
        desc->nb_objects = i;
        free_drm_prime_descriptor(NULL, (uint8_t *)desc);
        av_frame_free(&frame);
        return NULL;
      }
      desc->objects[i].fd = fd;
      uint32_t plane_height = drm_plane_height(dmabuf->fourcc, dmabuf->height, i);
      desc->objects[i].size =
          dmabuf->planes[i].offset + dmabuf->planes[i].stride * plane_height;
      desc->objects[i].format_modifier = dmabuf->modifier;
      desc->nb_objects = i + 1;
    }

    desc->nb_layers = 1;
    desc->layers[0].format = dmabuf->fourcc;
    desc->layers[0].nb_planes = dmabuf->nb_planes;
    for (int i = 0; i < dmabuf->nb_planes; ++i) {
      desc->layers[0].planes[i].object_index = i;
      desc->layers[0].planes[i].offset = dmabuf->planes[i].offset;
      desc->layers[0].planes[i].pitch = dmabuf->planes[i].stride;
    }

    frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc),
                                     free_drm_prime_descriptor, NULL, 0);
    if (!frame->buf[0]) {
      free_drm_prime_descriptor(NULL, (uint8_t *)desc);
      av_frame_free(&frame);
      LOG_ERROR(std::string("create_drm_prime_frame: av_buffer_create failed"));
      return NULL;
    }
    frame->data[0] = frame->buf[0]->data;
    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = dmabuf->encode_width;
    frame->height = dmabuf->encode_height;
    frame->hw_frames_ctx = create_drm_hw_frames_ctx(dmabuf);
    if (!frame->hw_frames_ctx) {
      av_frame_free(&frame);
      return NULL;
    }
    return frame;
  }

  AVBufferRef *create_drm_hw_frames_ctx(const FfmpegDmabufFrame *dmabuf) {
    if (!drm_device_ctx_) {
      LOG_ERROR(std::string("create_drm_hw_frames_ctx: DRM device is unavailable"));
      return NULL;
    }
    AVPixelFormat sw_format = drm_fourcc_to_av_pix_fmt(dmabuf->fourcc);
    if (sw_format == AV_PIX_FMT_NONE) {
      LOG_ERROR(std::string("create_drm_hw_frames_ctx: unsupported DRM fourcc ") +
                std::to_string(dmabuf->fourcc));
      return NULL;
    }

    AVBufferRef *frames_ref = av_hwframe_ctx_alloc(drm_device_ctx_);
    if (!frames_ref) {
      LOG_ERROR(std::string("create_drm_hw_frames_ctx: av_hwframe_ctx_alloc failed"));
      return NULL;
    }
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_DRM_PRIME;
    frames_ctx->sw_format = sw_format;
    frames_ctx->width = dmabuf->encode_width;
    frames_ctx->height = dmabuf->encode_height;
    frames_ctx->initial_pool_size = 0;
    int ret = av_hwframe_ctx_init(frames_ref);
    if (ret < 0) {
      LOG_ERROR(std::string("create_drm_hw_frames_ctx: av_hwframe_ctx_init failed, ret = ") +
                av_err2str(ret));
      av_buffer_unref(&frames_ref);
      return NULL;
    }
    return frames_ref;
  }

  AVBufferRef *create_vaapi_hw_frames_ctx(AVPixelFormat sw_format) {
    if (!hw_device_ctx_) {
      LOG_ERROR(std::string("create_vaapi_hw_frames_ctx: VAAPI device is unavailable"));
      return NULL;
    }

    AVBufferRef *frames_ref = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!frames_ref) {
      LOG_ERROR(std::string("create_vaapi_hw_frames_ctx: av_hwframe_ctx_alloc failed"));
      return NULL;
    }
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)frames_ref->data;
    frames_ctx->format = hw_pixfmt_;
    frames_ctx->sw_format = sw_format;
    frames_ctx->width = width_;
    frames_ctx->height = height_;
    frames_ctx->initial_pool_size = 0;
    int ret = av_hwframe_ctx_init(frames_ref);
    if (ret < 0) {
      LOG_ERROR(std::string("create_vaapi_hw_frames_ctx: av_hwframe_ctx_init failed, ret = ") +
                av_err2str(ret));
      av_buffer_unref(&frames_ref);
      return NULL;
    }
    return frames_ref;
  }

  AVPixelFormat drm_fourcc_to_av_pix_fmt(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FOURCC_CODE('N', 'V', '1', '2'):
      return AV_PIX_FMT_NV12;
    case DRM_FOURCC_CODE('X', 'R', '2', '4'):
      return AV_PIX_FMT_BGR0;
    case DRM_FOURCC_CODE('A', 'R', '2', '4'):
      return AV_PIX_FMT_BGRA;
    case DRM_FOURCC_CODE('X', 'B', '2', '4'):
      return AV_PIX_FMT_RGB0;
    case DRM_FOURCC_CODE('A', 'B', '2', '4'):
      return AV_PIX_FMT_RGBA;
    default:
      return AV_PIX_FMT_NONE;
    }
  }

  int map_dmabuf_to_hw_frame(AVFrame *drm_frame, AVFrame *mapped_frame,
                             AVBufferRef *hw_frames_ctx, int flags) {
    mapped_frame->format = hw_pixfmt_;
    mapped_frame->width = width_;
    mapped_frame->height = height_;
    mapped_frame->color_range = c_->color_range;
    mapped_frame->color_primaries = c_->color_primaries;
    mapped_frame->color_trc = c_->color_trc;
    mapped_frame->colorspace = c_->colorspace;
    mapped_frame->chroma_location = c_->chroma_sample_location;
    mapped_frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    if (!mapped_frame->hw_frames_ctx) {
      LOG_ERROR(std::string("map_dmabuf_to_hw_frame: av_buffer_ref failed"));
      return -1;
    }
    return av_hwframe_map(mapped_frame, drm_frame, flags);
  }

#ifdef RUSTDESK_HAS_AVFILTER
  int filter_vaapi_to_nv12(AVFrame *input_frame, AVFrame *output_frame) {
    AVFilterGraph *graph = avfilter_graph_alloc();
    if (!graph) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: avfilter_graph_alloc failed"));
      return -1;
    }

    AVFilterContext *src_ctx = NULL;
    AVFilterContext *scale_ctx = NULL;
    AVFilterContext *sink_ctx = NULL;
    int ret = 0;

    const AVFilter *src = avfilter_get_by_name("buffer");
    const AVFilter *scale = avfilter_get_by_name("scale_vaapi");
    const AVFilter *sink = avfilter_get_by_name("buffersink");
    if (!src || !scale || !sink) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: required filters are unavailable"));
      ret = -1;
      goto _exit;
    }

    if ((ret = avfilter_graph_create_filter(&src_ctx, src, "in", NULL, NULL, graph)) < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: create buffer failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }
    {
      AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
      if (!params) {
        LOG_ERROR(std::string("filter_vaapi_to_nv12: av_buffersrc_parameters_alloc failed"));
        ret = -1;
        goto _exit;
      }
      params->format = input_frame->format;
      params->width = input_frame->width;
      params->height = input_frame->height;
      params->time_base = AVRational{1, 1000};
      params->sample_aspect_ratio = AVRational{1, 1};
      params->hw_frames_ctx = input_frame->hw_frames_ctx;
      ret = av_buffersrc_parameters_set(src_ctx, params);
      av_free(params);
      if (ret < 0) {
        LOG_ERROR(std::string("filter_vaapi_to_nv12: parameters set failed, ret = ") +
                  av_err2str(ret));
        goto _exit;
      }
      if ((ret = avfilter_init_str(src_ctx, NULL)) < 0) {
        LOG_ERROR(std::string("filter_vaapi_to_nv12: buffer init failed, ret = ") +
                  av_err2str(ret));
        goto _exit;
      }
    }

    if ((ret = avfilter_graph_create_filter(&scale_ctx, scale, "scale",
                                            "format=nv12", NULL, graph)) < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: create scale_vaapi failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }
    scale_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    if (!scale_ctx->hw_device_ctx) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: av_buffer_ref hw device failed"));
      ret = -1;
      goto _exit;
    }

    if ((ret = avfilter_graph_create_filter(&sink_ctx, sink, "out", NULL, NULL, graph)) < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: create buffersink failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }

    if ((ret = avfilter_link(src_ctx, 0, scale_ctx, 0)) < 0 ||
        (ret = avfilter_link(scale_ctx, 0, sink_ctx, 0)) < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: link filters failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }
    if ((ret = avfilter_graph_config(graph, NULL)) < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: graph config failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }
    if ((ret = av_buffersrc_add_frame_flags(src_ctx, input_frame,
                                            AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: add frame failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }
    ret = av_buffersink_get_frame(sink_ctx, output_frame);
    if (ret < 0) {
      LOG_ERROR(std::string("filter_vaapi_to_nv12: get frame failed, ret = ") +
                av_err2str(ret));
      goto _exit;
    }

  _exit:
    avfilter_graph_free(&graph);
    return ret;
  }
#endif
#endif

  int fill_frame(AVFrame *frame, uint8_t *data, int data_length,
                 const int *const offset) {
    switch (frame->format) {
    case AV_PIX_FMT_NV12:
      if (data_length <
          frame->height * (frame->linesize[0] + frame->linesize[1] / 2)) {
        LOG_ERROR(std::string("fill_frame: NV12 data length error. data_length:") +
                  std::to_string(data_length) +
                  ", linesize[0]:" + std::to_string(frame->linesize[0]) +
                  ", linesize[1]:" + std::to_string(frame->linesize[1]));
        return -1;
      }
      frame->data[0] = data;
      frame->data[1] = data + offset[0];
      break;
    case AV_PIX_FMT_YUV420P:
      if (data_length <
          frame->height * (frame->linesize[0] + frame->linesize[1] / 2 +
                           frame->linesize[2] / 2)) {
        LOG_ERROR(std::string("fill_frame: 420P data length error. data_length:") +
                  std::to_string(data_length) +
                  ", linesize[0]:" + std::to_string(frame->linesize[0]) +
                  ", linesize[1]:" + std::to_string(frame->linesize[1]) +
                  ", linesize[2]:" + std::to_string(frame->linesize[2]));
        return -1;
      }
      frame->data[0] = data;
      frame->data[1] = data + offset[0];
      frame->data[2] = data + offset[1];
      break;
    default:
      LOG_ERROR(std::string("fill_frame: unsupported format, ") +
                std::to_string(frame->format));
      return -1;
    }
    return 0;
  }
};

} // namespace

extern "C" FFmpegRamEncoder *
ffmpeg_ram_new_encoder(const char *name, const char *mc_name, int width,
                       int height, int pixfmt, int align, int fps, int gop,
                       int rc, int quality, int kbs, int q, int thread_count,
                       int gpu, int *linesize, int *offset, int *length,
                       RamEncodeCallback callback) {
  FFmpegRamEncoder *encoder = NULL;
  try {
    encoder = new FFmpegRamEncoder(name, mc_name, width, height, pixfmt, align,
                                   fps, gop, rc, quality, kbs, q, thread_count,
                                   gpu, callback);
    if (encoder) {
      if (encoder->init(linesize, offset, length)) {
        return encoder;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("new FFmpegRamEncoder failed, ") + std::string(e.what()));
  }
  if (encoder) {
    encoder->free_encoder();
    delete encoder;
    encoder = NULL;
  }
  return NULL;
}

extern "C" int ffmpeg_ram_encode(FFmpegRamEncoder *encoder, const uint8_t *data,
                                 int length, const void *obj, uint64_t ms) {
  try {
    return encoder->encode(data, length, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_encode failed, ") + std::string(e.what()));
  }
  return -1;
}

extern "C" int ffmpeg_ram_encode_dmabuf(FFmpegRamEncoder *encoder,
                                        const FfmpegDmabufFrame *frame,
                                        const void *obj, uint64_t ms) {
  try {
    return encoder->encode_dmabuf(frame, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_encode_dmabuf failed, ") +
              std::string(e.what()));
  }
  return -1;
}

extern "C" void ffmpeg_ram_free_encoder(FFmpegRamEncoder *encoder) {
  try {
    if (!encoder)
      return;
    encoder->free_encoder();
    delete encoder;
    encoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("free encoder failed, ") + std::string(e.what()));
  }
}

extern "C" int ffmpeg_ram_set_bitrate(FFmpegRamEncoder *encoder, int kbs) {
  try {
    return encoder->set_bitrate(kbs);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_set_bitrate failed, ") + std::string(e.what()));
  }
  return -1;
}
