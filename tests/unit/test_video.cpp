/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// ffmpeg includes
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// local includes
#include <src/config.h>
#include <src/video.h>

using namespace std::literals;

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    BaseTest::SetUp();
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#if defined(__linux__) || defined(__FreeBSD__)
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

/**
 * @brief Parameterized coverage for effective H.264 profile selection.
 */
struct H264ProfileTest: testing::TestWithParam<std::tuple<std::string_view, video::amf::coder_e, int, int>> {};

TEST_P(H264ProfileTest, SelectProfile) {
  const auto &[encoder_name, coder, chroma_sampling_type, expected_profile] = GetParam();
  video::config_t config {};
  config.chromaSamplingType = chroma_sampling_type;

  EXPECT_EQ(expected_profile, video::select_h264_profile(encoder_name, config, std::to_underlying(coder)));
}

INSTANTIATE_TEST_SUITE_P(
  H264ProfileTests,
  H264ProfileTest,
  testing::Values(
    std::make_tuple("h264_amf"sv, video::amf::coder_e::auto_, 0, AV_PROFILE_H264_HIGH),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cabac, 0, AV_PROFILE_H264_HIGH),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cavlc, 0, AV_PROFILE_H264_CONSTRAINED_BASELINE),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cavlc, 1, AV_PROFILE_H264_HIGH_444_PREDICTIVE),
    std::make_tuple("h264_nvenc"sv, video::amf::coder_e::cavlc, 0, AV_PROFILE_H264_HIGH)
  )
);

/**
 * @brief Parameterized coverage for resolving requested dynamic range against encoder capabilities.
 */
struct DynamicRangeTest: testing::TestWithParam<std::tuple<int, int, int, bool, bool, int>> {};

TEST_P(DynamicRangeTest, Resolve) {
  const auto &[video_format, chroma_sampling_type, requested_dynamic_range, supports_hdr, supports_hdr_yuv444, expected_dynamic_range] = GetParam();

  video::encoder_t encoder {
    "test"sv,
    {},
    {},
    {},
    {},
    0,
  };
  encoder.h264.name = "h264_test";
  encoder.hevc.name = "hevc_test";
  encoder.av1.name = "av1_test";

  video::config_t config {};
  config.videoFormat = video_format;
  config.dynamicRange = requested_dynamic_range;
  config.chromaSamplingType = chroma_sampling_type;

  auto *codec = &encoder.h264;
  if (video_format == 1) {
    codec = &encoder.hevc;
  } else if (video_format == 2) {
    codec = &encoder.av1;
  }
  (*codec)[video::encoder_t::DYNAMIC_RANGE] = supports_hdr;
  (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = supports_hdr_yuv444;

  const auto effective_config = video::resolve_dynamic_range(encoder, config);

  EXPECT_EQ(expected_dynamic_range, effective_config.dynamicRange);
  EXPECT_EQ(requested_dynamic_range, config.dynamicRange);
  EXPECT_EQ(video_format, effective_config.videoFormat);
  EXPECT_EQ(chroma_sampling_type, effective_config.chromaSamplingType);
}

INSTANTIATE_TEST_SUITE_P(
  DynamicRangeTests,
  DynamicRangeTest,
  testing::Values(
    std::make_tuple(0, 0, 1, false, true, 0),
    std::make_tuple(1, 0, 1, false, true, 0),
    std::make_tuple(1, 0, 1, true, false, 1),
    std::make_tuple(2, 1, 1, true, false, 0),
    std::make_tuple(2, 1, 1, false, true, 1),
    std::make_tuple(1, 0, 0, false, false, 0)
  )
);

#ifdef _WIN32
TEST(AmfH264OptionsTest, CoderUsesConfiguredValue) {
  const auto coder_option = std::ranges::find(video::amdvce.h264.common_options, "coder"sv, &video::encoder_t::option_t::name);

  ASSERT_NE(video::amdvce.h264.common_options.end(), coder_option);
  ASSERT_TRUE(std::holds_alternative<int *>(coder_option->value));
  EXPECT_EQ(&config::video.amd.amd_coder, std::get<int *>(coder_option->value));
}

/**
 * @brief Parameterized coverage for the AMF maximum access-unit-size option mappings.
 */
struct AmfMaxAuSizeOptionsTest: testing::TestWithParam<std::tuple<const video::encoder_t::codec_t *, bool>> {};

TEST_P(AmfMaxAuSizeOptionsTest, UsesConfiguredValueForSupportedCodecsOnly) {
  const auto &[codec, supported] = GetParam();
  const auto option = std::ranges::find(codec->common_options, "max_au_size"sv, &video::encoder_t::option_t::name);

  if (!supported) {
    EXPECT_EQ(codec->common_options.end(), option);
    return;
  }

  ASSERT_NE(codec->common_options.end(), option);
  ASSERT_TRUE(std::holds_alternative<std::optional<int> *>(option->value));
  EXPECT_EQ(&config::video.amd.amd_max_au_size, std::get<std::optional<int> *>(option->value));
}

INSTANTIATE_TEST_SUITE_P(
  AmfCodecOptions,
  AmfMaxAuSizeOptionsTest,
  testing::Values(
    std::make_tuple(&video::amdvce.h264, true),
    std::make_tuple(&video::amdvce.hevc, true),
    std::make_tuple(&video::amdvce.av1, false)
  )
);

/**
 * @brief Parameterized coverage for the QuickSync option tables.
 */
struct QsvCodecOptionsTest: testing::TestWithParam<const video::encoder_t::codec_t *> {};

TEST_P(QsvCodecOptionsTest, TunableOptionsBindToConfig) {
  const auto *codec = GetParam();

  const auto extbrc = std::ranges::find(codec->common_options, "extbrc"sv, &video::encoder_t::option_t::name);
  ASSERT_NE(codec->common_options.end(), extbrc);
  ASSERT_TRUE(std::holds_alternative<std::optional<int> *>(extbrc->value));
  EXPECT_EQ(&config::video.qsv.qsv_extbrc, std::get<std::optional<int> *>(extbrc->value));

  const auto max_frame_size = std::ranges::find(codec->common_options, "max_frame_size"sv, &video::encoder_t::option_t::name);
  ASSERT_NE(codec->common_options.end(), max_frame_size);
  ASSERT_TRUE(std::holds_alternative<std::optional<int> *>(max_frame_size->value));
  EXPECT_EQ(&config::video.qsv.qsv_max_frame_size, std::get<std::optional<int> *>(max_frame_size->value));
}

TEST_P(QsvCodecOptionsTest, FallbackOptionsChangeTheConfiguration) {
  const auto *codec = GetParam();

  // A fallback set that merely repeats a common option wastes the single retry
  // in make_avcodec_encode_session() on an identical configuration.
  for (const auto &fallback : codec->fallback_options) {
    const auto common = std::ranges::find(codec->common_options, fallback.name, &video::encoder_t::option_t::name);
    if (common == codec->common_options.end()) {
      continue;
    }

    if (const auto *common_value = std::get_if<int>(&common->value)) {
      if (const auto *fallback_value = std::get_if<int>(&fallback.value)) {
        EXPECT_NE(*common_value, *fallback_value)
          << "fallback option '" << fallback.name << "' for " << codec->name
          << " repeats the value already set in common_options";
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
  QsvCodecOptions,
  QsvCodecOptionsTest,
  testing::Values(
    &video::quicksync.h264,
    &video::quicksync.hevc,
    &video::quicksync.av1
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

/**
 * @brief The scenario hint exists on h264_qsv/hevc_qsv but not on av1_qsv.
 */
TEST(QsvScenarioTest, AppliedOnlyToCodecsThatSupportIt) {
  const auto has_scenario = [](const video::encoder_t::codec_t &codec) {
    return std::ranges::find(codec.common_options, "scenario"sv, &video::encoder_t::option_t::name) != codec.common_options.end();
  };

  EXPECT_TRUE(has_scenario(video::quicksync.h264));
  EXPECT_TRUE(has_scenario(video::quicksync.hevc));
  EXPECT_FALSE(has_scenario(video::quicksync.av1));
}
#endif

/**
 * @brief Saves and restores global configuration around encoder option parsing tests.
 *
 * Derived fixtures reset the option under test in `SetUp()` and then parse a
 * configuration snippet with `config::apply_config_for_test()`.
 */
struct EncoderConfigTest: BaseTest {
  void SetUp() override {
    BaseTest::SetUp();
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/test_video.cpp";
  }

  void TearDown() override {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified_config_settings;
    BaseTest::TearDown();
  }

  config::video_t original_video {config::video};  ///< Video configuration restored after each test.
  config::audio_t original_audio {config::audio};  ///< Audio configuration restored after each test.
  config::stream_t original_stream {config::stream};  ///< Stream configuration restored after each test.
  config::nvhttp_t original_nvhttp {config::nvhttp};  ///< HTTP configuration restored after each test.
  config::input_t original_input {config::input};  ///< Input configuration restored after each test.
  config::sunshine_t original_sunshine {config::sunshine};  ///< Core configuration restored after each test.
  decltype(config::modified_config_settings) original_modified_config_settings {config::modified_config_settings};  ///< Modified settings restored after each test.
};

using QsvPresetConfigParam = std::tuple<std::string_view, std::optional<int>>;

/**
 * @brief Parameterized coverage for parsing the QuickSync preset.
 */
struct QsvPresetConfigTest: EncoderConfigTest, testing::WithParamInterface<QsvPresetConfigParam> {
  void SetUp() override {
    EncoderConfigTest::SetUp();
    config::video.qsv.qsv_preset.reset();
  }
};

TEST_P(QsvPresetConfigTest, ParsesEveryValueTheUiCanEmit) {
  const auto &[setting, expected] = GetParam();
  config::apply_config_for_test(setting);

  EXPECT_EQ(expected, config::video.qsv.qsv_preset);
}

// Every value here must stay in sync with the qsv_preset <option> list in
// src_assets/common/assets/web/configs/tabs/encoders/IntelQuickSyncEncoder.vue.
// A preset the UI offers but the parser rejects is silently dropped instead of
// applied, which is exactly the failure this suite guards against.
INSTANTIATE_TEST_SUITE_P(
  QsvPresetValues,
  QsvPresetConfigTest,
  testing::Values(
    QsvPresetConfigParam {"qsv_preset = veryfast\n"sv, 7},
    QsvPresetConfigParam {"qsv_preset = faster\n"sv, 6},
    QsvPresetConfigParam {"qsv_preset = fast\n"sv, 5},
    QsvPresetConfigParam {"qsv_preset = medium\n"sv, 4},
    QsvPresetConfigParam {"qsv_preset = slow\n"sv, 3},
    QsvPresetConfigParam {"qsv_preset = slower\n"sv, 2},
    QsvPresetConfigParam {"qsv_preset = veryslow\n"sv, 1},
    // Legacy value previously written by the web UI.
    QsvPresetConfigParam {"qsv_preset = slowest\n"sv, 1},
    QsvPresetConfigParam {"qsv_preset = bogus\n"sv, std::nullopt}
  )
);

using QsvMaxFrameSizeConfigParam = std::tuple<std::string_view, std::optional<int>>;

/**
 * @brief Parameterized coverage for parsing and validating the QuickSync maximum frame size.
 */
struct QsvMaxFrameSizeConfigTest: EncoderConfigTest, testing::WithParamInterface<QsvMaxFrameSizeConfigParam> {
  void SetUp() override {
    EncoderConfigTest::SetUp();
    config::video.qsv.qsv_max_frame_size.reset();
  }
};

TEST_P(QsvMaxFrameSizeConfigTest, AcceptsOnlyPositiveValues) {
  const auto &[setting, expected] = GetParam();
  config::apply_config_for_test(setting);

  EXPECT_EQ(expected, config::video.qsv.qsv_max_frame_size);
}

INSTANTIATE_TEST_SUITE_P(
  QsvMaxFrameSizeValues,
  QsvMaxFrameSizeConfigTest,
  testing::Values(
    QsvMaxFrameSizeConfigParam {""sv, std::nullopt},
    QsvMaxFrameSizeConfigParam {"qsv_max_frame_size = 0\n"sv, std::nullopt},
    QsvMaxFrameSizeConfigParam {"qsv_max_frame_size = -1\n"sv, std::nullopt},
    QsvMaxFrameSizeConfigParam {"qsv_max_frame_size = 1\n"sv, 1},
    QsvMaxFrameSizeConfigParam {"qsv_max_frame_size = 131072\n"sv, 131072}
  )
);

using QsvExtbrcConfigParam = std::tuple<std::string_view, std::optional<int>>;

/**
 * @brief Parameterized coverage for parsing the QuickSync extended bitrate control mode.
 */
struct QsvExtbrcConfigTest: EncoderConfigTest, testing::WithParamInterface<QsvExtbrcConfigParam> {
  void SetUp() override {
    EncoderConfigTest::SetUp();
    config::video.qsv.qsv_extbrc.reset();
  }
};

TEST_P(QsvExtbrcConfigTest, LeavesDriverDefaultUnlessExplicitlySet) {
  const auto &[setting, expected] = GetParam();
  config::apply_config_for_test(setting);

  EXPECT_EQ(expected, config::video.qsv.qsv_extbrc);
}

INSTANTIATE_TEST_SUITE_P(
  QsvExtbrcValues,
  QsvExtbrcConfigTest,
  testing::Values(
    QsvExtbrcConfigParam {""sv, std::nullopt},
    QsvExtbrcConfigParam {"qsv_extbrc = auto\n"sv, std::nullopt},
    QsvExtbrcConfigParam {"qsv_extbrc = enabled\n"sv, 1},
    QsvExtbrcConfigParam {"qsv_extbrc = disabled\n"sv, 0}
  )
);

using AmfMaxAuSizeConfigParam = std::tuple<std::string_view, std::optional<int>>;

/**
 * @brief Parameterized coverage for parsing and validating the AMF maximum access-unit size.
 */
struct AmfMaxAuSizeConfigTest: EncoderConfigTest, testing::WithParamInterface<AmfMaxAuSizeConfigParam> {
  void SetUp() override {
    EncoderConfigTest::SetUp();
    config::video.amd.amd_max_au_size.reset();
  }
};

TEST_P(AmfMaxAuSizeConfigTest, AcceptsOnlyFfmpegSupportedRange) {
  const auto &[setting, expected] = GetParam();
  config::apply_config_for_test(setting);

  EXPECT_EQ(expected, config::video.amd.amd_max_au_size);
}

INSTANTIATE_TEST_SUITE_P(
  AmfMaxAuSizeValues,
  AmfMaxAuSizeConfigTest,
  testing::Values(
    AmfMaxAuSizeConfigParam {""sv, std::nullopt},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = -2\n"sv, std::nullopt},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = -1\n"sv, -1},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 0\n"sv, 0},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 800000\n"sv, 800000},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 2147483647\n"sv, std::numeric_limits<int>::max()}
  )
);

struct FramerateX100Test: BaseTest, testing::WithParamInterface<std::tuple<std::int32_t, AVRational>> {};

TEST_P(FramerateX100Test, Run) {
  const auto &[x100, expected] = GetParam();
  auto res = video::framerateX100_to_rational(x100);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, AVRational {24000, 1001}),
    std::make_tuple(2398, AVRational {24000, 1001}),
    std::make_tuple(2500, AVRational {25, 1}),
    std::make_tuple(2997, AVRational {30000, 1001}),
    std::make_tuple(3000, AVRational {30, 1}),
    std::make_tuple(5994, AVRational {60000, 1001}),
    std::make_tuple(6000, AVRational {60, 1}),
    std::make_tuple(11988, AVRational {120000, 1001}),
    std::make_tuple(23976, AVRational {240000, 1001}),  // future NTSC 240hz?
    std::make_tuple(9498, AVRational {4749, 50})  // from my LG 27GN950
  )
);

struct FramerateToRationalTest: testing::TestWithParam<std::tuple<int, int, AVRational>> {};

TEST_P(FramerateToRationalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  auto res = video::framerate_to_rational(config);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateToRationalTests,
  FramerateToRationalTest,
  testing::Values(
    std::make_tuple(60, 0, AVRational {60, 1}),  // no X100 value, fall back to integer framerate
    std::make_tuple(60, 5994, AVRational {60000, 1001}),
    std::make_tuple(120, 11988, AVRational {120000, 1001}),
    std::make_tuple(24, 2398, AVRational {24000, 1001})
  )
);

struct CaptureFrameIntervalTest: testing::TestWithParam<std::tuple<int, int, std::chrono::nanoseconds>> {};

TEST_P(CaptureFrameIntervalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  ASSERT_EQ(expected, video::capture_frame_interval(config));
}

INSTANTIATE_TEST_SUITE_P(
  CaptureFrameIntervalTests,
  CaptureFrameIntervalTest,
  testing::Values(
    std::make_tuple(60, 0, std::chrono::nanoseconds {16666666}),
    std::make_tuple(60, 5994, std::chrono::nanoseconds {16683333}),  // 1e9 * 1001 / 60000
    std::make_tuple(120, 11988, std::chrono::nanoseconds {8341666})  // 1e9 * 1001 / 120000
  )
);

/**
 * @brief Software encoder converts BGR0 and NV12 frames, including padded strides and
 *        backends that don't report the pixel pitch.
 */
TEST(SoftwareEncoderConversion, Bgr0AndNv12) {
  constexpr int w = 320;
  constexpr int h = 240;

  AVFrame *frame = av_frame_alloc();
  ASSERT_NE(frame, nullptr);
  frame->width = w;
  frame->height = h;
  frame->format = AV_PIX_FMT_YUV420P;

  video::avcodec_software_encode_device_t device;
  ASSERT_EQ(device.init(w, h, frame, AV_PIX_FMT_YUV420P, false), 0);
  // set_frame() takes ownership of the frame; the device frees it on destruction.
  ASSERT_EQ(device.set_frame(frame, nullptr), 0);

  // BGR0 frame (4 bytes per pixel) -- the classic KMS/DMABUF capture layout.
  std::vector<uint8_t> bgr0_buffer(static_cast<size_t>(w) * h * 4);
  platf::img_t bgr0_img {};
  bgr0_img.data = bgr0_buffer.data();
  bgr0_img.width = w;
  bgr0_img.height = h;
  bgr0_img.row_pitch = w * 4;
  bgr0_img.pixel_pitch = 4;
  EXPECT_EQ(device.convert(bgr0_img), 0);

  // NV12 frame (1 byte per pixel row pitch, Y plane + interleaved UV) -- the
  // layout delivered by PipeWire-based captures (KWin screencast / portal).
  std::vector<uint8_t> nv12_buffer(static_cast<size_t>(w) * h + static_cast<size_t>(w) * h / 2);
  platf::img_t nv12_img {};
  nv12_img.data = nv12_buffer.data();
  nv12_img.width = w;
  nv12_img.height = h;
  nv12_img.row_pitch = w;
  nv12_img.pixel_pitch = 1;
  EXPECT_EQ(device.convert(nv12_img), 0);

  // Padded-stride NV12 (alignment padding) -- the case the old row_pitch ==
  // width heuristic misdetected as BGR0, causing out-of-bounds reads.
  constexpr int padded_stride = w + 32;
  std::vector<uint8_t> padded_nv12_buffer(static_cast<size_t>(padded_stride) * h + static_cast<size_t>(padded_stride) * h / 2);
  platf::img_t padded_nv12_img {};
  padded_nv12_img.data = padded_nv12_buffer.data();
  padded_nv12_img.width = w;
  padded_nv12_img.height = h;
  padded_nv12_img.row_pitch = padded_stride;
  padded_nv12_img.pixel_pitch = 1;
  EXPECT_EQ(device.convert(padded_nv12_img), 0);

  // Capture backends that don't report pixel_pitch fall back to deriving it
  // from the row pitch (1 byte per pixel = NV12, 4 = BGR0).
  platf::img_t fallback_bgr0_img {};
  fallback_bgr0_img.data = bgr0_buffer.data();
  fallback_bgr0_img.width = w;
  fallback_bgr0_img.height = h;
  fallback_bgr0_img.row_pitch = w * 4;
  EXPECT_EQ(device.convert(fallback_bgr0_img), 0);

  platf::img_t fallback_nv12_img {};
  fallback_nv12_img.data = nv12_buffer.data();
  fallback_nv12_img.width = w;
  fallback_nv12_img.height = h;
  fallback_nv12_img.row_pitch = w;
  EXPECT_EQ(device.convert(fallback_nv12_img), 0);
}
