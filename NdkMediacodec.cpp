#include "media/NdkMediaCodec.h"
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

#include <signal.h>

static bool g_loop = true;

struct EncoderParms {
  int w = 1920;
  int h = 1080;
  int bitrate = 0;
};

static void handleSignal(int signo)
{
    if (SIGINT == signo || SIGTSTP == signo)
    {
        printf("\033[0;31mprogram termination abnormally!\033[0;39m\n");
        g_loop = false;
    }
    // exit(-1);
}

template <typename T>
T SwapEndian(T u) {
  return u;
  union {
    T u;
    unsigned char u8[sizeof(T)];
  } source, dest;

  source.u = u;

  for (size_t k = 0; k < sizeof(T); k++) dest.u8[k] = source.u8[sizeof(T) - k - 1];

  return dest.u;
}

std::vector<uint8_t> GetIVFVP8Header(int w, int h, int timebase) {
  std::vector<uint8_t> output(32, 0);

  // 0-3
  memcpy(&output[0], "DKIF", 4);
  // 4-5 0
  // 6-7 hearder size
  *(int16_t*)(&output[6]) = SwapEndian<int16_t>(32);
  // 8-11 type
  memcpy(&output[8], "VP80", 4);
  // 12-13 width
  *(int16_t*)(&output[12]) = SwapEndian<int16_t>(w);
  // 14-15 height
  *(int16_t*)(&output[14]) = SwapEndian<int16_t>(h);
  // 16-19 timebase numerator
  *(int32_t*)(&output[16]) = SwapEndian<int32_t>(timebase);
  // 20-23 timebase denominator
  *(int32_t*)(&output[20]) = SwapEndian<int32_t>(1);
  // 24-25 frames
  *(int16_t*)(&output[24]) = SwapEndian<int16_t>(500);
  // 26-31 no use

  return output;
}

std::vector<uint8_t> GetIVFFrameHeader(int size, int64_t pts) {
  std::vector<uint8_t> output(12, 0);
  // 0-3 size
  *(int32_t*)(&output[0]) = SwapEndian<int32_t>(size);
  // 4-11 pts
  *(int64_t*)(&output[4]) = SwapEndian<int64_t>(pts);

  return output;
}

int64_t getNowUs() {
  timeval tv;
  gettimeofday(&tv, 0);
  return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

void Encode(const std::string& mime, const std::string& input_file, const EncoderParms& parms) {
  AMediaCodec* media_codec = nullptr;
  AMediaFormat* format = nullptr;

  FILE* fp_in = nullptr;
  FILE* fp_out = nullptr;

  fp_in = fopen(input_file.c_str(), "r");
  if (!fp_in) {
    printf("open %s failed\n", input_file.c_str());
    return;
  }

  std::string out_file_name = "/data/local/tmp/output.ivf";
  fp_out = fopen(out_file_name.c_str(), "w+");
  if (!fp_out) {
    printf("open %s failed\n", out_file_name.c_str());
    return;
  } else {
    printf("open %s success\n", out_file_name.c_str());
  }

  // std::string mime = "video/x-vnd.on2.vp8";
  // std::string mime = "video/avc";
  media_codec = AMediaCodec_createEncoderByType(mime.c_str());
  if (!media_codec) {
    printf("create codec err\n");
    return;
  }

  format = AMediaFormat_new();

  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime.c_str());
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, parms.w);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, parms.h);
  // https://www.androidos.net.cn/android/9.0.0_r8/xref/frameworks/native/headers/media_plugin/media/openmax/OMX_IVCommon.h
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 60);
  // AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, -1);    // don't set -1 on rk3588 Android 14
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 100000);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BITRATE_MODE, 2);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, parms.bitrate);

  // 这里配置 format
  media_status_t status =
      AMediaCodec_configure(media_codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  if (status != 0) {
    printf("erro config %d\n", status);
    return;
  }

  AMediaCodec_start(media_codec);
  if (status != 0) {
    printf("start erro %d\n", status);
    return;
  }

  int send_count = 0;
  int recv_count = 0;

  auto header = GetIVFVP8Header(parms.w, parms.h, 1000000);
  fwrite(&header[0], header.size(), 1, fp_out);
  fflush(fp_out);

  int frameLen = parms.w * parms.h * 3 / 2;
  uint8_t frame_buffer[frameLen];

  while (g_loop) {
    int64_t pts = 16777 * send_count + 1;

    if (send_count % 60 == 0) {
      AMediaFormat* idr_fmt = AMediaFormat_new();
      printf("request-sync\n");
      AMediaFormat_setInt32(idr_fmt, "request-sync", 0);
      AMediaCodec_setParameters(media_codec, idr_fmt);
      AMediaFormat_delete(idr_fmt);
    }

    if (fread(frame_buffer, 1, frameLen, fp_in) < frameLen) {
      if (feof(fp_in)) {
        printf("---- eof ---\n");
        fseek(fp_in, 0, SEEK_SET);
      }
      // break;
    }

    // 请求buffer
    ssize_t bufidx = AMediaCodec_dequeueInputBuffer(media_codec, 10000);
    if (bufidx >= 0) {
      size_t bufsize;
      uint8_t* buf = AMediaCodec_getInputBuffer(media_codec, bufidx, &bufsize);
      memcpy(buf, frame_buffer, frameLen);
      send_count++;
      // printf("send_count: %d\n", send_count);
      // 填充数据
      AMediaCodec_queueInputBuffer(media_codec, bufidx, 0, frameLen, pts, 0);
    }

    AMediaCodecBufferInfo info;
    // 取输出buffer
    auto outindex = AMediaCodec_dequeueOutputBuffer(media_codec, &info, 1000000);
    // while (outindex < 0 && g_loop) {
    //   // printf("no output %ld\n", outindex);
    //   outindex = AMediaCodec_dequeueOutputBuffer(media_codec, &info, 1000000);
    // }
    if (outindex > 0) {
      // 在这里取走编码后的数据
      // 释放buffer给编码器
      size_t outsize = 0;
      uint8_t* buf = AMediaCodec_getOutputBuffer(media_codec, outindex, &outsize);
      auto frame_pkt = GetIVFFrameHeader(info.size, recv_count * 16777 + 1);
      fwrite(&frame_pkt[0], 1, frame_pkt.size(), fp_out);
      if (info.flags == AMEDIACODEC_CONFIGURE_FLAG_ENCODE) {
        printf("flags: %d, idr\n", info.flags);
      }
      if (info.flags != 2) {
        recv_count++;
      }
      fwrite(buf, 1, info.size, fp_out);
      fflush(fp_out);
      AMediaCodec_releaseOutputBuffer(media_codec, outindex, false);
    }
  }

  // input empty frame
  AMediaCodec_queueInputBuffer(media_codec, 0, 0, 0, 0, 4);

  AMediaCodecBufferInfo info;
  // 取输出buffer
  auto outindex = AMediaCodec_dequeueOutputBuffer(media_codec, &info, 1000000);
  // while (outindex >= 0 && g_loop) {  // have bug: F libc    : Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 24393 (HwBinder:24387_), pid 24387 (codec_demo)
  while (outindex >= 0) {
    if (info.flags == AMEDIACODEC_CONFIGURE_FLAG_ENCODE) {
      printf("flags: %d, idr\n", info.flags);
    }
    // 在这里取走编码后的数据
    // 释放buffer给编码器
    size_t outsize = 0;
    uint8_t* buf = AMediaCodec_getOutputBuffer(media_codec, outindex, &outsize);
    auto frame_pkt = GetIVFFrameHeader(info.size, recv_count * 16777 + 1);
    fwrite(&frame_pkt[0], 1, frame_pkt.size(), fp_out);
    recv_count++;
    fwrite(buf, 1, info.size, fp_out);
    fflush(fp_out);
    AMediaCodec_releaseOutputBuffer(media_codec, outindex, false);

    outindex = AMediaCodec_dequeueOutputBuffer(media_codec, &info, 1000000);
  }

  if (send_count != recv_count) {
    printf("%d %d\n", send_count, recv_count);
  }

  fclose(fp_in);
  fclose(fp_out);
}

int main(int argc, char* argv[]) {
  EncoderParms parms;
  std::string input_file;
  std::string code_type;

  signal(SIGINT, handleSignal);
  signal(SIGTERM, handleSignal);

  struct option long_options[] = {{"width", required_argument, 0, 1920},
                                  {"height", required_argument, 0, 1080},
                                  {"bitrate", required_argument, 0, 4000000},
                                  {"file", required_argument, 0, 0},
                                  {"codeType", required_argument, 0, 0},
                                  {0, 0, 0, 0}};

  int index = -1;
  while (getopt_long_only(argc, argv, "", long_options, &index) != -1) {
    switch (index) {
      case 0:
        parms.w = atoi(optarg);
        break;
      case 1:
        parms.h = atoi(optarg);
        break;
      case 2:
        parms.bitrate = atoi(optarg);
        break;
      case 3:
        input_file = optarg;
        break;
      case 4:
        code_type = optarg;
        break;
      default:
        break;
    }
  }

  printf("codeType: %s, width: %d, height: %d, bitrate: %d, file: %s\n", code_type.c_str(), parms.w, parms.h, parms.bitrate, input_file.c_str());

  if (input_file.empty()) {
    printf("no input file\n");
    return 0;
  }

  Encode(code_type, input_file, parms);

  return 0;
}
