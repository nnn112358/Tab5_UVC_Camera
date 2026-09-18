/*
 * UltraTinyOD (UHD: Ultra lightweight Human Detection) の ESP-DL ラッパー
 *
 * 元実装: https://github.com/PINTO0309/esp-who/tree/custom/examples/ultra_lightweight_human_detection
 *         (components/uhd_detect) を esp-dl 3.3 の API に合わせて移植したもの
 * モデル:  https://github.com/PINTO0309/UHD
 */
#pragma once

#include "dl_detect_base.hpp"

namespace uhd_detect {

class UltraLightweightHumanDetect : public dl::detect::DetectImpl {
public:
    static inline constexpr float default_score_thr = 0.15f;
    static inline constexpr float default_nms_thr   = 0.45f;
    static inline constexpr int   default_top_k     = 10;

    UltraLightweightHumanDetect(float score_thr = default_score_thr,
                                float nms_thr   = default_nms_thr,
                                int   top_k     = default_top_k);
    ~UltraLightweightHumanDetect() override;

    bool ok() const { return m_model && m_image_preprocessor && m_postprocessor; }

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override;

    // 直近の run() の所要時間 (ms)
    float last_pre_ms   = 0;
    float last_infer_ms = 0;
    float last_post_ms  = 0;
};

} // namespace uhd_detect
