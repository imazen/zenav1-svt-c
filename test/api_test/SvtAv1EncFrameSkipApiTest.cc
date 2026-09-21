/*
 * Copyright(c) 2026 Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license/software-license. If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * https://www.aomedia.org/license/patent-license.
 */

/******************************************************************************
 * @file SvtAv1EncFrameSkipApiTest.cc
 *
 * @brief Regression tests for pre-encode frame skipping in RTC low-delay CBR.
 ******************************************************************************/

#include <cstdint>
#include <vector>

#include "EbSvtAv1.h"
#include "EbSvtAv1Enc.h"
#include "gtest/gtest.h"

namespace {

constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 240;
constexpr uint32_t kFrameBytes = kWidth * kHeight * 3 / 2;

class Encoder {
  public:
    Encoder() {
        EXPECT_EQ(EB_ErrorNone, svt_av1_enc_init_handle(&handle_, &config_));
        default_target_bit_rate_ = config_.target_bit_rate;

        config_.enc_mode = 9;
        config_.tune = 1;
        config_.rtc = true;
        config_.level_of_parallelism = 1;
        config_.intra_period_length = -1;
        config_.hierarchical_levels = 0;
        config_.pred_structure = LOW_DELAY;
        config_.source_width = kWidth;
        config_.source_height = kHeight;
        config_.frame_rate_numerator = 30;
        config_.frame_rate_denominator = 1;
        config_.encoder_bit_depth = 8;
        config_.encoder_color_format = EB_YUV420;
        config_.rate_control_mode = SVT_AV1_RC_MODE_CBR;
        config_.target_bit_rate = 10000;
        config_.max_qp_allowed = 63;
        config_.min_qp_allowed = 4;
        config_.under_shoot_pct = 100;
        config_.over_shoot_pct = 100;
        config_.maximum_buffer_size_ms = 600;
        config_.starting_buffer_level_ms = 599;
        config_.optimal_buffer_level_ms = 599;
        config_.look_ahead_distance = 0;
        config_.enable_overlays = false;
        config_.scene_change_detection = 0;
        config_.recode_loop = 4;
    }

    ~Encoder() {
        if (handle_ == nullptr)
            return;
        if (initialized_) {
            EbBufferHeaderType eos{};
            eos.size = sizeof(eos);
            eos.flags = EB_BUFFERFLAG_EOS;
            eos.pic_type = EB_AV1_INVALID_PICTURE;
            svt_av1_enc_send_picture(handle_, &eos);
            svt_av1_enc_deinit(handle_);
        }
        svt_av1_enc_deinit_handle(handle_);
    }

    EbSvtAv1EncConfiguration& config() {
        return config_;
    }

    uint32_t default_target_bit_rate() const {
        return default_target_bit_rate_;
    }

    EbErrorType set_parameter() {
        return svt_av1_enc_set_parameter(handle_, &config_);
    }

    EbErrorType init() {
        const EbErrorType ret = svt_av1_enc_init(handle_);
        initialized_ = ret == EB_ErrorNone;
        return ret;
    }

    EbErrorType send(EbAv1PictureType pic_type,
                     EbPrivDataNode* private_data = nullptr) {
        fill_frame();
        io_.luma = y_.data();
        io_.cb = u_.data();
        io_.cr = v_.data();
        io_.y_stride = kWidth;
        io_.cb_stride = kWidth / 2;
        io_.cr_stride = kWidth / 2;

        EbBufferHeaderType input{};
        input.size = sizeof(input);
        input.p_buffer = reinterpret_cast<uint8_t*>(&io_);
        input.n_alloc_len = input.n_filled_len = kFrameBytes;
        input.pts = pts_++;
        input.pic_type = pic_type;
        input.p_app_private = private_data;

        const EbErrorType ret = svt_av1_enc_send_picture(handle_, &input);
        if (ret == EB_ErrorNone)
            drain_one_picture();
        return ret;
    }

  private:
    void fill_frame() {
        for (size_t i = 0; i < y_.size(); ++i)
            y_[i] = static_cast<uint8_t>(
                (i * 131 + pts_ * 17 + i / kWidth * 29) & 0xff);
        for (size_t i = 0; i < u_.size(); ++i) {
            u_[i] = static_cast<uint8_t>((i * 67 + pts_ * 23) & 0xff);
            v_[i] = static_cast<uint8_t>((i * 43 + pts_ * 31) & 0xff);
        }
    }

    void drain_one_picture() {
        bool stop = false;
        while (!stop) {
            EbBufferHeaderType* output = nullptr;
            ASSERT_EQ(EB_ErrorNone,
                      svt_av1_enc_get_packet(handle_, &output, 0));
            ASSERT_NE(nullptr, output);
            stop = (output->flags & EB_BUFFERFLAG_EOS) ||
                   !(output->flags & EB_BUFFERFLAG_IS_ALT_REF);
            svt_av1_enc_release_out_buffer(&output);
        }
    }

    EbComponentType* handle_ = nullptr;
    EbSvtAv1EncConfiguration config_{};
    uint32_t default_target_bit_rate_ = 0;
    bool initialized_ = false;
    int64_t pts_ = 0;
    EbSvtIOFormat io_{};
    std::vector<uint8_t> y_ = std::vector<uint8_t>(kWidth * kHeight);
    std::vector<uint8_t> u_ = std::vector<uint8_t>(kWidth * kHeight / 4);
    std::vector<uint8_t> v_ = std::vector<uint8_t>(kWidth * kHeight / 4);
};

TEST(SvtAv1FrameSkipApiTest, DefaultIsDisabled) {
    EbComponentType* handle = nullptr;
    EbSvtAv1EncConfiguration config{};
    config.max_allowed_consecutive_frames_skips = 0xff;
    ASSERT_EQ(EB_ErrorNone, svt_av1_enc_init_handle(&handle, &config));
    EXPECT_EQ(0, config.max_allowed_consecutive_frames_skips);
    EXPECT_EQ(EB_ErrorNone, svt_av1_enc_deinit_handle(handle));
}

TEST(SvtAv1FrameSkipApiTest, AcceptedOnlyForRtcLowDelayCbr) {
    Encoder valid;
    valid.config().max_allowed_consecutive_frames_skips = 2;
    EXPECT_EQ(EB_ErrorNone, valid.set_parameter());

    Encoder non_rtc;
    non_rtc.config().rtc = false;
    non_rtc.config().max_allowed_consecutive_frames_skips = 2;
    EXPECT_EQ(EB_ErrorBadParameter, non_rtc.set_parameter());

    Encoder non_cbr;
    non_cbr.config().rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
    non_cbr.config().target_bit_rate = non_cbr.default_target_bit_rate();
    non_cbr.config().max_allowed_consecutive_frames_skips = 2;
    EXPECT_EQ(EB_ErrorBadParameter, non_cbr.set_parameter());
}

TEST(SvtAv1FrameSkipApiTest, ReturnsSuccessStatusAndCapsConsecutiveSkips) {
    Encoder encoder;
    encoder.config().max_allowed_consecutive_frames_skips = 2;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_INVALID_PICTURE));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
}

TEST(SvtAv1FrameSkipApiTest, SupportsTwoTemporalLayers) {
    Encoder encoder;
    encoder.config().hierarchical_levels = 1;
    encoder.config().max_allowed_consecutive_frames_skips = 1;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_INVALID_PICTURE));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
}

TEST(SvtAv1FrameSkipApiTest, KeyFramePreservesCreditForNextInterFrame) {
    Encoder encoder;
    encoder.config().max_allowed_consecutive_frames_skips = 1;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));
    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
}

TEST(SvtAv1FrameSkipApiTest, MgSizeCommandPreservesCreditForNextInterFrame) {
    Encoder encoder;
    encoder.config().max_allowed_consecutive_frames_skips = 1;
    encoder.config().max_hierarchical_levels = 1;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));
    SvtAv1MgSizeInfo info{.hierarchical_levels = 1};
    EbPrivDataNode node{
        .node_type = MG_SIZE_CHANGE_EVENT,
        .data = &info,
        .size = sizeof(info),
        .next = nullptr,
    };
    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_INVALID_PICTURE, &node));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
}

TEST(SvtAv1FrameSkipApiTest, RefMgmtCommandPreservesCreditForNextInterFrame) {
    Encoder encoder;
    encoder.config().max_allowed_consecutive_frames_skips = 1;
    encoder.config().max_managed_refs = 1;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));
    SvtAv1RefFrameCmd command{.pic_id = 1};
    EbPrivDataNode node{
        .node_type = REF_STORE_EVENT,
        .data = &command,
        .size = sizeof(command),
        .next = nullptr,
    };
    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_INVALID_PICTURE, &node));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
}

TEST(SvtAv1FrameSkipApiTest, AppliesDynamicCommandsOnSkippedFrame) {
    Encoder encoder;
    encoder.config().max_allowed_consecutive_frames_skips = 1;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));

    SvtAv1RateInfo rate{.seq_qp = 0, .target_bit_rate = 20000};
    SvtAv1FrameRateInfo frame_rate{
        .frame_rate_numerator = 15,
        .frame_rate_denominator = 1,
    };
    SvtAv1PresetInfo preset{.enc_mode = 10};
    EbPrivDataNode preset_node{
        .node_type = PRESET_CHANGE_EVENT,
        .data = &preset,
        .size = sizeof(preset),
        .next = nullptr,
    };
    EbPrivDataNode frame_rate_node{
        .node_type = FRAME_RATE_CHANGE_EVENT,
        .data = &frame_rate,
        .size = sizeof(frame_rate),
        .next = &preset_node,
    };
    EbPrivDataNode rate_node{
        .node_type = RATE_CHANGE_EVENT,
        .data = &rate,
        .size = sizeof(rate),
        .next = &frame_rate_node,
    };

    EXPECT_EQ(EB_NoErrorFrameSkipped,
              encoder.send(EB_AV1_INVALID_PICTURE, &rate_node));
    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_INVALID_PICTURE));
}

TEST(SvtAv1FrameSkipApiTest,
     RefFrameScalingCommandPreservesCreditForNextInterFrame) {
    Encoder encoder;
    encoder.config().max_allowed_consecutive_frames_skips = 1;
    ASSERT_EQ(EB_ErrorNone, encoder.set_parameter());
    ASSERT_EQ(EB_ErrorNone, encoder.init());

    ASSERT_EQ(EB_ErrorNone, encoder.send(EB_AV1_KEY_PICTURE));

    EbRefFrameScale scale{
        .scale_mode = RESIZE_FIXED,
        .scale_denom = 12,
        .scale_kf_denom = 12,
    };
    EbPrivDataNode node{
        .node_type = REF_FRAME_SCALING_EVENT,
        .data = &scale,
        .size = sizeof(scale),
        .next = nullptr,
    };

    EXPECT_EQ(EB_ErrorNone, encoder.send(EB_AV1_INVALID_PICTURE, &node));
    EXPECT_EQ(EB_NoErrorFrameSkipped, encoder.send(EB_AV1_INVALID_PICTURE));
}

}  // namespace
