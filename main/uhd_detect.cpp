/*
 * UltraTinyOD (UHD) の ESP-DL ラッパー (esp-dl 3.3 向け)
 *
 * 元実装: https://github.com/PINTO0309/esp-who/tree/custom/examples/ultra_lightweight_human_detection
 */
#include "uhd_detect.hpp"

#include "dl_image_preprocessor.hpp"
#include "dl_tensor_base.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "uhd_constants.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace {
constexpr char kTag[] = "uhd_detect";

// main/CMakeLists.txt の EMBED_FILES で埋め込んだ models/uhd_w32.espdl
extern "C" {
extern const uint8_t _binary_uhd_w32_espdl_start[];
extern const uint8_t _binary_uhd_w32_espdl_end[];
}
// アンカー定数の選択に使うモデル名 (uhd_constants.hpp の get_uhd_anchor_set 参照)
constexpr char kModelName[] = "ultratinyod_anc8_w32_64x64_opencv_inter_nearest_static_nopost";

static inline float sigmoid_f(float x)
{
    if (x >= 80.0f) return 1.0f;
    if (x <= -80.0f) return 0.0f;
    return 1.0f / (1.0f + std::exp(-x));
}

static inline float softplus_f(float x)
{
    if (x > 20.0f) return x;
    if (x < -20.0f) return std::exp(x);
    return std::log1p(std::exp(x));
}

// 出力 "box" (tx,ty,tw,th x アンカー数) と "quality" (アンカー数) をデコードする後処理
class UhdPostprocessor : public dl::detect::DetectPostprocessor {
public:
    UhdPostprocessor(dl::Model *model, dl::image::ImagePreprocessor *pre, float score_thr, float nms_thr, int top_k)
        : dl::detect::DetectPostprocessor(model, pre, score_thr, nms_thr, top_k)
    {
        if (!uhd_detect::get_uhd_anchor_set(kModelName, &m_anchor_set)) {
            m_anchor_set = {nullptr, nullptr, 0};
        }
    }

    void postprocess() override
    {
        dl::TensorBase *box     = m_model->get_output("box");
        dl::TensorBase *quality = m_model->get_output("quality");
        if (!box || !quality) {
            ESP_LOGE(kTag, "missing output tensor(s)");
            return;
        }
        if (box->shape.size() != 4 || quality->shape.size() != 4) {
            ESP_LOGE(kTag, "unexpected output dims");
            return;
        }
        bool nhwc = (box->shape[3] % 4 == 0) && (quality->shape[3] == box->shape[3] / 4);
        bool nchw = (box->shape[1] % 4 == 0) && (quality->shape[1] == box->shape[1] / 4);
        if (!nhwc && !nchw) {
            ESP_LOGE(kTag, "output shape mismatch (box=%s quality=%s)",
                     dl::vector_to_string(box->shape).c_str(), dl::vector_to_string(quality->shape).c_str());
            return;
        }
        if (!m_anchor_set.anchors || m_anchor_set.count <= 0) {
            ESP_LOGE(kTag, "anchors missing for model %s", kModelName);
            return;
        }

        if (box->dtype == dl::DATA_TYPE_INT8) {
            parse_maps<int8_t>(box, quality, nhwc);
        } else if (box->dtype == dl::DATA_TYPE_INT16) {
            parse_maps<int16_t>(box, quality, nhwc);
        } else if (box->dtype == dl::DATA_TYPE_FLOAT) {
            parse_maps<float>(box, quality, nhwc);
        } else {
            ESP_LOGE(kTag, "unsupported output dtype: %s", dl::dtype_to_string(box->dtype));
            return;
        }
        nms();
    }

private:
    template <typename T>
    void parse_maps(dl::TensorBase *box, dl::TensorBase *quality, bool nhwc)
    {
        const int H     = nhwc ? box->shape[1] : box->shape[2];
        const int W     = nhwc ? box->shape[2] : box->shape[3];
        const int na    = nhwc ? quality->shape[3] : quality->shape[1];
        const int box_c = nhwc ? box->shape[3] : box->shape[1];
        if (box_c != na * 4) {
            ESP_LOGE(kTag, "box channel mismatch: box_c=%d anchors=%d", box_c, na);
            return;
        }
        if (na != m_anchor_set.count) {
            ESP_LOGW(kTag, "anchor count mismatch: model=%d const=%d", na, m_anchor_set.count);
        }

        auto *box_ptr     = static_cast<T *>(box->data);
        auto *quality_ptr = static_cast<T *>(quality->data);

        constexpr bool is_float = std::is_floating_point_v<T>;
        const float box_scale     = is_float ? 1.f : DL_SCALE(box->exponent.get());
        const float quality_scale = is_float ? 1.f : DL_SCALE(quality->exponent.get());
        auto val = [&](T *p, size_t i, float scale) -> float {
            if constexpr (is_float) return static_cast<float>(p[i]);
            else return dl::dequantize(p[i], scale);
        };

        // モデル入力 (64x64) 座標 → 元画像座標への換算
        float inv_sx = m_image_preprocessor->get_resize_scale_x(true);
        float inv_sy = m_image_preprocessor->get_resize_scale_y(true);
        int   tl_x   = m_image_preprocessor->get_crop_area_top_left_x();
        int   tl_y   = m_image_preprocessor->get_crop_area_top_left_y();
        dl::TensorBase *input = m_image_preprocessor->get_model_input();
        float scale_w = static_cast<float>(input->shape[2]) * inv_sx;
        float scale_h = static_cast<float>(input->shape[1]) * inv_sy;

        const size_t HW = static_cast<size_t>(H) * W;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                for (int a = 0; a < na; ++a) {
                    size_t idx_q = nhwc ? ((static_cast<size_t>(y) * W + x) * na + a)
                                        : ((static_cast<size_t>(a) * H + y) * W + x);
                    float score = sigmoid_f(val(quality_ptr, idx_q, quality_scale));
                    if (score < m_score_thr) continue;

                    float tx, ty, tw, th;
                    if (nhwc) {
                        size_t b = (static_cast<size_t>(y) * W + x) * na * 4 + a * 4;
                        tx = val(box_ptr, b + 0, box_scale);
                        ty = val(box_ptr, b + 1, box_scale);
                        tw = val(box_ptr, b + 2, box_scale);
                        th = val(box_ptr, b + 3, box_scale);
                    } else {
                        size_t b = static_cast<size_t>(a) * 4 * HW + static_cast<size_t>(y) * W + x;
                        tx = val(box_ptr, b + 0 * HW, box_scale);
                        ty = val(box_ptr, b + 1 * HW, box_scale);
                        tw = val(box_ptr, b + 2 * HW, box_scale);
                        th = val(box_ptr, b + 3 * HW, box_scale);
                    }

                    float anchor_w = m_anchor_set.anchors[a * 2]     * m_anchor_set.wh_scale[a * 2];
                    float anchor_h = m_anchor_set.anchors[a * 2 + 1] * m_anchor_set.wh_scale[a * 2 + 1];

                    float cx = (sigmoid_f(tx) + x) / static_cast<float>(W);
                    float cy = (sigmoid_f(ty) + y) / static_cast<float>(H);
                    float bw = anchor_w * softplus_f(tw);
                    float bh = anchor_h * softplus_f(th);

                    float x1 = (cx - 0.5f * bw) * scale_w + tl_x;
                    float y1 = (cy - 0.5f * bh) * scale_h + tl_y;
                    float x2 = (cx + 0.5f * bw) * scale_w + tl_x;
                    float y2 = (cy + 0.5f * bh) * scale_h + tl_y;
                    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) continue;
                    if (x2 <= x1 || y2 <= y1) continue;

                    dl::detect::result_t r = {0, score, {(int)x1, (int)y1, (int)x2, (int)y2}, {}};
                    m_box_list.insert(std::upper_bound(m_box_list.begin(), m_box_list.end(), r, dl::detect::greater_box), r);
                }
            }
        }
    }

    uhd_detect::UhdAnchorSet m_anchor_set;
};
} // namespace

namespace uhd_detect {

UltraLightweightHumanDetect::UltraLightweightHumanDetect(float score_thr, float nms_thr, int top_k)
{
    m_model = nullptr;
    m_image_preprocessor = nullptr;
    m_postprocessor = nullptr;

    size_t size = _binary_uhd_w32_espdl_end - _binary_uhd_w32_espdl_start;
    ESP_LOGI(kTag, "model=%s (%u bytes)", kModelName, (unsigned)size);

    // param_copy=true: パラメータを flash から RAM にコピーして実行
    m_model = new dl::Model(reinterpret_cast<const char *>(_binary_uhd_w32_espdl_start),
                            fbs::MODEL_LOCATION_IN_FLASH_RODATA, 0, dl::MEMORY_MANAGER_GREEDY, nullptr, true);
    if (!m_model) {
        ESP_LOGE(kTag, "model allocation failed");
        return;
    }
    m_model->minimize();

    // 入力は RGB 画像 (0..255 をそのまま量子化)。rgb_swap=false = モデルは RGB 順を期待
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255}, false);
    m_postprocessor = new UhdPostprocessor(m_model, m_image_preprocessor, score_thr, nms_thr, top_k);
}

UltraLightweightHumanDetect::~UltraLightweightHumanDetect() {}

std::list<dl::detect::result_t> &UltraLightweightHumanDetect::run(const dl::image::img_t &img)
{
    static std::list<dl::detect::result_t> empty;
    if (!ok()) {
        ESP_LOGE(kTag, "model not initialized");
        return empty;
    }
    int64_t t0 = esp_timer_get_time();
    m_image_preprocessor->preprocess(img);
    int64_t t1 = esp_timer_get_time();
    m_model->run();
    int64_t t2 = esp_timer_get_time();
    m_postprocessor->clear_result();
    m_postprocessor->postprocess();
    auto &result = m_postprocessor->get_result(img.width, img.height);
    int64_t t3 = esp_timer_get_time();

    last_pre_ms   = (t1 - t0) / 1000.0f;
    last_infer_ms = (t2 - t1) / 1000.0f;
    last_post_ms  = (t3 - t2) / 1000.0f;
    return result;
}

} // namespace uhd_detect
