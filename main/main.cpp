/*
 * Tab5_UVC_Camera
 * M5Stack Tab5 (ESP32-P4) : USB カメラ (UVC) の映像を M5Unified で画面に表示する
 *
 * Author: nnn112358
 *
 *  - USB-A ポートに接続した UVC カメラを espressif/usb_host_uvc で取り込む
 *  - MJPEG は ESP32-P4 内蔵のハードウェア JPEG デコーダで RGB565 に展開
 *  - YUY2 (非圧縮) しか持たないカメラはソフトウェアで RGB565 に変換
 *  - ESP32-P4 の PPA (Pixel Processing Accelerator) で回転 + 拡大し、
 *    M5GFX (Panel_DSI) のフレームバッファへ直接書き込む (CPU コピー無し)
 *    ※ M5GFX の setRotation(1) + pushImage は PSRAM フレームバッファへ
 *      1 ピクセルずつ回転書き込みするため 1280x720 で約 1 fps しか出ない
 *  - fps 表示などの文字は M5.Display で上書き描画
 *
 *  実測 (640x480 MJPEG): デコード 4ms + PPA 19ms → 30 fps
 */

#include <M5Unified.h>

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"

static const char *TAG = "tab5_uvc";

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------
#define PREF_FRAME_W        640         // カメラに要求する解像度 (MJPEG 優先)
#define PREF_FRAME_H        480
#define MAX_FRAME_W         1280        // デコード先バッファの上限
#define MAX_FRAME_H         720
#define FIT_TO_SCREEN       1           // 1: 画面に合わせて拡大 (アスペクト維持), 0: 等倍で中央表示
#define SHOW_FPS            1           // 1: 左上に fps を表示
#define NUM_FRAME_BUFFERS   3           // UVC ドライバが持つフレームバッファ数
#define SELF_TEST           0           // 1: 起動時に JPEG 色順 / PPA 回転方向の自己診断ログを出す

// パネル (縦向き) の物理サイズ
#define PANEL_W             720
#define PANEL_H             1280
// 横向き表示 (M5GFX setRotation(1) 相当) にするための PPA 回転角。
// PPA は反時計回り指定なので、時計回り 90 度 = 270。
#define LANDSCAPE_ROTATION  PPA_SRM_ROTATION_ANGLE_270

// ---------------------------------------------------------------------------
// 共有状態
// ---------------------------------------------------------------------------
struct connected_info_t {
    uint8_t dev_addr;
    uint8_t stream_index;
    size_t  frame_info_num;
};

static QueueHandle_t      s_connect_q      = nullptr;   // 接続イベント通知
static QueueHandle_t      s_frame_q        = nullptr;   // 受信フレーム (uvc_host_frame_t*)
static SemaphoreHandle_t  s_disconnect_sem = nullptr;
static SemaphoreHandle_t  s_proc_mutex     = nullptr;   // フレーム処理中のロック
static uvc_host_stream_hdl_t s_stream      = nullptr;
static volatile bool      s_stream_active  = false;

static jpeg_decoder_handle_t s_jpeg_dec = nullptr;
static uint8_t  *s_rgb_buf      = nullptr;   // デコード結果 (横向き RGB565)
static size_t    s_rgb_buf_size = 0;

static ppa_client_handle_t s_ppa = nullptr;
static uint8_t  *s_disp_buf      = nullptr;  // PPA 出力 (縦向き RGB565, フレームバッファ直書きできない時用)
static size_t    s_disp_buf_size = 0;
static uint8_t  *s_fb            = nullptr;  // M5GFX (DSI) のフレームバッファ 720x1280 RGB565
static size_t    s_fb_size       = 0;

static int64_t s_t_decode = 0, s_t_ppa = 0, s_t_push = 0;   // 計測用 (us 累積)

// ---------------------------------------------------------------------------
// USB ホストライブラリのイベント処理タスク
// ---------------------------------------------------------------------------
static void usb_lib_task(void *arg)
{
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: all devices freed");
        }
    }
}

// ---------------------------------------------------------------------------
// UVC ドライバ / ストリームのコールバック
// ---------------------------------------------------------------------------
static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        connected_info_t info = {
            .dev_addr       = event->device_connected.dev_addr,
            .stream_index   = event->device_connected.uvc_stream_index,
            .frame_info_num = event->device_connected.frame_info_num,
        };
        ESP_LOGI(TAG, "UVC device connected: addr=%u stream=%u formats=%u",
                 info.dev_addr, info.stream_index, (unsigned)info.frame_info_num);
        xQueueSend(s_connect_q, &info, 0);
    }
}

static void uvc_stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB transfer error: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "UVC device disconnected");
        xSemaphoreGive(s_disconnect_sem);
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "frame buffer overflow (frame too large)");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGD(TAG, "frame buffer underflow (display too slow, frame dropped)");
        break;
    default:
        break;
    }
}

// UVC ドライバのタスクから呼ばれる。ここでは重い処理をせずキューに渡す。
// false を返すとフレームは我々が所有し、後で uvc_host_frame_return() で返す。
static bool uvc_frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    if (xQueueSendToBack(s_frame_q, &frame, 0) != pdPASS) {
        return true;    // キューが満杯なら処理済み扱いで即返却
    }
    return false;
}

// ---------------------------------------------------------------------------
// 画像変換
// ---------------------------------------------------------------------------
static inline uint16_t yuv_to_rgb565(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void yuy2_to_rgb565(const uint8_t *src, uint16_t *dst, int w, int h)
{
    const int n = w * h / 2;
    for (int i = 0; i < n; ++i) {
        int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        dst[0] = yuv_to_rgb565(y0, u, v);
        dst[1] = yuv_to_rgb565(y1, u, v);
        src += 4;
        dst += 2;
    }
}

// JPEG → RGB565 (ハードウェアデコーダ)。成功時 w/h を返す。
static bool decode_jpeg(const uint8_t *data, size_t len, uint8_t *out, size_t out_size, int &w, int &h)
{
    jpeg_decode_picture_info_t info;
    esp_err_t err = jpeg_decoder_get_info(data, len, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg header parse failed: %s", esp_err_to_name(err));
        return false;
    }
    if (info.width * info.height * 2 > out_size) {
        ESP_LOGW(TAG, "jpeg %ux%u too large for buffer", (unsigned)info.width, (unsigned)info.height);
        return false;
    }
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        // BGR = リトルエンディアン RGB565 (uint16_t = RRRRRGGGGGGBBBBB)
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t written = 0;
    err = jpeg_decoder_process(s_jpeg_dec, &cfg, data, len, out, out_size, &written);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return false;
    }
    w = info.width;
    h = info.height;
    return true;
}

// ---------------------------------------------------------------------------
// PPA: 横向き画像 (w x h) を回転 + 拡大して、縦向きの出力ピクチャ (dst_w x dst_h) の中央へ書く。
// 出力ブロックのサイズと位置を返す。
// ---------------------------------------------------------------------------
static bool ppa_rotate_scale(const uint8_t *src, int w, int h, float zoom,
                             uint8_t *dst, size_t dst_size, int dst_w, int dst_h,
                             int &blk_w, int &blk_h, int &blk_x, int &blk_y)
{
    // PPA の倍率は 1/16 刻み (ドライバと同じ丸め方で出力サイズを求める)
    uint32_t zi = (uint32_t)zoom;
    uint32_t zf = (uint32_t)(zoom * 16) & 15;
    if (zi == 0 && zf == 0) zf = 1;
    int sw = (int)(zi * w + zf * w / 16);   // 拡大後 (横向き) 幅
    int sh = (int)(zi * h + zf * h / 16);   // 拡大後 (横向き) 高さ
    blk_w = sh;                             // 回転後 (縦向き)
    blk_h = sw;
    if (blk_w > dst_w || blk_h > dst_h) {
        return false;
    }
    blk_x = ((dst_w - blk_w) / 2) & ~1;
    blk_y = (dst_h - blk_h) / 2;

    ppa_srm_oper_config_t op = {};
    op.in.buffer          = src;
    op.in.pic_w           = w;
    op.in.pic_h           = h;
    op.in.block_w         = w;
    op.in.block_h         = h;
    op.in.block_offset_x  = 0;
    op.in.block_offset_y  = 0;
    op.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer         = dst;
    op.out.buffer_size    = dst_size;
    op.out.pic_w          = dst_w;
    op.out.pic_h          = dst_h;
    op.out.block_offset_x = blk_x;
    op.out.block_offset_y = blk_y;
    op.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle     = LANDSCAPE_ROTATION;
    op.scale_x            = zoom;
    op.scale_y            = zoom;
    op.alpha_update_mode  = PPA_ALPHA_NO_CHANGE;
    op.mode               = PPA_TRANS_MODE_BLOCKING;

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa srm failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 描画
// ---------------------------------------------------------------------------
static void draw_frame(const uint8_t *rgb, int w, int h)
{
    auto &disp = M5.Display;

#if FIT_TO_SCREEN
    float zoom = std::min((float)PANEL_H / w, (float)PANEL_W / h);   // 横向き: 幅 1280 / 高さ 720
#else
    float zoom = 1.0f;
#endif
    int bw = 0, bh = 0, bx = 0, by = 0;
    int64_t t0 = esp_timer_get_time();
    if (s_fb) {
        // PPA で DSI フレームバッファへ直接書く (コピー無し)
        if (!ppa_rotate_scale(rgb, w, h, zoom, s_fb, s_fb_size, PANEL_W, PANEL_H, bw, bh, bx, by)) {
            return;
        }
        s_t_ppa += esp_timer_get_time() - t0;
    } else {
        if (!ppa_rotate_scale(rgb, w, h, zoom, s_disp_buf, s_disp_buf_size, PANEL_W, PANEL_H, bw, bh, bx, by)) {
            return;
        }
        int64_t t1 = esp_timer_get_time();
        s_t_ppa += t1 - t0;
        disp.startWrite();
        disp.pushImage(0, 0, PANEL_W, PANEL_H, (const uint16_t *)s_disp_buf);
        disp.endWrite();
        s_t_push += esp_timer_get_time() - t1;
    }

#if SHOW_FPS
    static uint32_t frames = 0;
    static uint32_t last_ms = 0;
    static float fps = 0.0f;
    ++frames;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - last_ms >= 1000) {
        fps = frames * 1000.0f / (now - last_ms);
        ESP_LOGI(TAG, "%dx%d -> %dx%d  %.1f fps  (decode %.1f ms, ppa %.1f ms, push %.1f ms)", w, h, bh, bw, fps,
                 s_t_decode / 1000.0f / frames, s_t_ppa / 1000.0f / frames, s_t_push / 1000.0f / frames);
        frames = 0;
        last_ms = now;
        s_t_decode = s_t_ppa = s_t_push = 0;
    }
    disp.startWrite();
    disp.setRotation(1);   // 文字だけ横向きで描く (小領域なので遅くない)
    disp.setTextColor(TFT_WHITE, TFT_BLACK);
    disp.setTextSize(2);
    disp.setCursor(4, 4);
    disp.printf(" %dx%d %.1f fps ", w, h, fps);
    disp.setRotation(0);
    disp.endWrite();
#endif
}

static void show_message(const char *msg)
{
    auto &disp = M5.Display;
    disp.setRotation(1);
    disp.fillScreen(TFT_BLACK);
    disp.setTextColor(TFT_WHITE, TFT_BLACK);
    disp.setTextSize(3);
    disp.setTextDatum(textdatum_t::middle_center);
    disp.drawString(msg, disp.width() / 2, disp.height() / 2);
    disp.setTextDatum(textdatum_t::top_left);
    disp.setRotation(0);
}

// ---------------------------------------------------------------------------
// フレーム処理タスク: キューから取り出し → デコード → 描画 → 返却
// ---------------------------------------------------------------------------
static void frame_processing_task(void *arg)
{
    while (true) {
        uvc_host_frame_t *frame = nullptr;
        if (xQueueReceive(s_frame_q, &frame, portMAX_DELAY) != pdPASS) {
            continue;
        }
        xSemaphoreTake(s_proc_mutex, portMAX_DELAY);
        if (s_stream_active) {
            int w = 0, h = 0;
            bool ok = false;
            switch (frame->vs_format.format) {
            case UVC_VS_FORMAT_MJPEG: {
                int64_t t0 = esp_timer_get_time();
                ok = decode_jpeg(frame->data, frame->data_len, s_rgb_buf, s_rgb_buf_size, w, h);
                s_t_decode += esp_timer_get_time() - t0;
                break;
            }
            case UVC_VS_FORMAT_YUY2:
                w = frame->vs_format.h_res;
                h = frame->vs_format.v_res;
                if ((size_t)(w * h * 2) <= s_rgb_buf_size && frame->data_len >= (size_t)(w * h * 2)) {
                    yuy2_to_rgb565(frame->data, (uint16_t *)s_rgb_buf, w, h);
                    ok = true;
                }
                break;
            default:
                break;
            }
            if (ok) {
                draw_frame(s_rgb_buf, w, h);
            }
            uvc_host_frame_return(s_stream, frame);
        }
        xSemaphoreGive(s_proc_mutex);
    }
}

// ---------------------------------------------------------------------------
// フォーマット選択: PREF サイズの MJPEG > 画面に収まる最大 MJPEG > YUY2 (640x480 以下)
// ---------------------------------------------------------------------------
static bool select_format(const connected_info_t &info, uvc_host_stream_format_t &out)
{
    size_t n = info.frame_info_num;
    if (n == 0) {
        return false;
    }
    uvc_host_frame_info_t (*list)[] = (uvc_host_frame_info_t (*)[])heap_caps_malloc(
        n * sizeof(uvc_host_frame_info_t), MALLOC_CAP_DEFAULT);
    if (!list) {
        return false;
    }
    size_t got = n;
    if (uvc_host_get_frame_list(info.dev_addr, info.stream_index, list, &got) != ESP_OK) {
        free(list);
        return false;
    }

    static const char *fmt_str[] = {"DEFAULT", "MJPEG", "YUY2", "H264", "H265", "NV12"};
    int best_score = -1;
    uvc_host_frame_info_t best = {};
    for (size_t i = 0; i < got; ++i) {
        const auto &f = (*list)[i];
        const char *fs = (f.format < 6) ? fmt_str[f.format] : "?";
        ESP_LOGI(TAG, "  format[%u]: %s %ux%u (interval %lu)", (unsigned)i, fs,
                 f.h_res, f.v_res, (unsigned long)f.default_interval);

        int score = -1;
        if (f.format == UVC_VS_FORMAT_MJPEG && f.h_res == PREF_FRAME_W && f.v_res == PREF_FRAME_H) {
            score = 2000000;
        } else if (f.format == UVC_VS_FORMAT_MJPEG && f.h_res <= MAX_FRAME_W && f.v_res <= MAX_FRAME_H) {
            score = 1000000 + (int)(f.h_res * f.v_res / 100);
        } else if (f.format == UVC_VS_FORMAT_YUY2 && f.h_res <= 640 && f.v_res <= 480) {
            score = (int)(f.h_res * f.v_res / 100);
        }
        if (score > best_score) {
            best_score = score;
            best = f;
        }
    }
    free(list);
    if (best_score < 0) {
        return false;
    }
    out.h_res  = best.h_res;
    out.v_res  = best.v_res;
    out.fps    = best.default_interval ? 10000000.0f / best.default_interval : 0.0f;
    out.format = best.format;
    return true;
}

// ---------------------------------------------------------------------------
// 自己診断 (シリアルログで色順と回転方向を確認する)
// ---------------------------------------------------------------------------
#if SELF_TEST
#include "test_jpg.h"   // 160x120: 左半分 赤 / 右半分 青
static void self_test()
{
    int w = 0, h = 0;
    // HW デコーダはフラッシュ (rodata) を直接読めないので RAM へコピー
    uint8_t *jpg = (uint8_t *)heap_caps_malloc(sizeof(test_jpg), MALLOC_CAP_SPIRAM);
    memcpy(jpg, test_jpg, sizeof(test_jpg));
    if (decode_jpeg(jpg, sizeof(test_jpg), s_rgb_buf, s_rgb_buf_size, w, h)) {
        const uint16_t *p = (const uint16_t *)s_rgb_buf;
        ESP_LOGI(TAG, "[selftest] jpeg %dx%d  red-pixel=0x%04X (expect F800)  blue-pixel=0x%04X (expect 001F)",
                 w, h, p[60 * w + 40], p[60 * w + 120]);
    } else {
        ESP_LOGE(TAG, "[selftest] test jpeg decode failed");
    }
    free(jpg);

    // 4x2 の入力: in[y][x] = y*16 + x
    uint16_t *in = (uint16_t *)s_rgb_buf;
    const int iw = 4, ih = 2;
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x)
            in[y * iw + x] = (uint16_t)(y * 16 + x);
    int ow = 0, oh = 0, ox = 0, oy = 0;
    if (ppa_rotate_scale(s_rgb_buf, iw, ih, 1.0f, s_disp_buf, s_disp_buf_size, ih, iw, ow, oh, ox, oy)) {
        const uint16_t *o = (const uint16_t *)s_disp_buf;
        bool ok = true;
        for (int py = 0; py < oh; ++py)
            for (int px = 0; px < ow; ++px)
                if (o[py * ow + px] != in[(ih - 1 - px) * iw + py]) ok = false;   // 期待: 時計回り 90 度
        ESP_LOGI(TAG, "[selftest] ppa out %dx%d: %02X %02X / %02X %02X / %02X %02X / %02X %02X  -> clockwise90 %s",
                 ow, oh, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], ok ? "OK" : "MISMATCH");
    }
}
#endif

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setColorDepth(16);
    // pushImage(uint16_t*) にリトルエンディアン RGB565 をそのまま渡す
    // (デフォルトの false ではバイトスワップ済みデータとして解釈され色が化ける)
    M5.Display.setSwapBytes(true);
    show_message("Connect a USB camera");

    // USB-A ポートへの 5V 給電を ON (Tab5: PI4IOE 0x44 pin3 = USB5V_EN)
    M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 同期オブジェクト
    s_connect_q      = xQueueCreate(2, sizeof(connected_info_t));
    s_frame_q        = xQueueCreate(NUM_FRAME_BUFFERS, sizeof(uvc_host_frame_t *));
    s_disconnect_sem = xSemaphoreCreateBinary();
    s_proc_mutex     = xSemaphoreCreateMutex();
    assert(s_connect_q && s_frame_q && s_disconnect_sem && s_proc_mutex);

    // ハードウェア JPEG デコーダ
    const jpeg_decode_engine_cfg_t dec_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 200,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&dec_cfg, &s_jpeg_dec));
    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_rgb_buf = (uint8_t *)jpeg_alloc_decoder_mem(MAX_FRAME_W * MAX_FRAME_H * 2, &mem_cfg, &s_rgb_buf_size);
    assert(s_rgb_buf);

    // PPA (回転 / 拡大) と出力バッファ (キャッシュライン 128B 境界)
    const ppa_client_config_t ppa_cfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ppa));
    s_disp_buf_size = (PANEL_W * PANEL_H * 2 + 127) & ~127;
    s_disp_buf = (uint8_t *)heap_caps_aligned_calloc(128, 1, s_disp_buf_size, MALLOC_CAP_SPIRAM);
    assert(s_disp_buf);

    // Tab5 の M5GFX は DSI パネル (Panel_DSI) なのでフレームバッファへ直接アクセスできる
    if (M5.getBoard() == m5::board_t::board_M5Tab5 || M5.getBoard() == m5::board_t::board_M5Tab5X) {
        auto panel = static_cast<lgfx::Panel_DSI *>(M5.Display.getPanel());
        auto &det = panel->config_detail();
        // (M5GFX は buffer_length を設定しないので、サイズはパネル寸法から決める)
        if (det.buffer && ((uintptr_t)det.buffer & 127) == 0) {
            s_fb      = (uint8_t *)det.buffer;
            s_fb_size = PANEL_W * PANEL_H * 2;
            ESP_LOGI(TAG, "DSI framebuffer %p (%u bytes): PPA writes directly", s_fb, (unsigned)s_fb_size);
        } else {
            ESP_LOGW(TAG, "DSI framebuffer not usable for PPA (%p, %u), fallback to pushImage",
                     det.buffer, (unsigned)det.buffer_length);
        }
    }

#if SELF_TEST
    self_test();
#endif

    // USB ホスト
    ESP_LOGI(TAG, "Installing USB Host");
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags     = ESP_INTR_FLAG_LOWMED;
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 10, nullptr, 0);

    // UVC ドライバ
    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority   = 6,
        .xCoreID                = 0,
        .create_background_task = true,
        .event_cb               = uvc_driver_event_cb,
        .user_ctx               = nullptr,
    };
    ESP_ERROR_CHECK(uvc_host_install(&uvc_driver_config));

    // デコード/描画タスク (USB 処理と別コアで実行)
    xTaskCreatePinnedToCore(frame_processing_task, "frame_proc", 8 * 1024, nullptr, 5, nullptr, 1);

    while (true) {
        connected_info_t info;
        xQueueReceive(s_connect_q, &info, portMAX_DELAY);

        uvc_host_stream_format_t fmt = {};
        if (!select_format(info, fmt)) {
            ESP_LOGW(TAG, "No usable MJPEG/YUY2 format found, using device default");
            fmt.format = UVC_VS_FORMAT_DEFAULT;
        }
        ESP_LOGI(TAG, "Selected: format=%d %ux%u @ %.1f fps", fmt.format, fmt.h_res, fmt.v_res, fmt.fps);

        size_t frame_size = (fmt.h_res && fmt.v_res) ? (size_t)fmt.h_res * fmt.v_res * 2 : 0;
        const uvc_host_stream_config_t stream_config = {
            .event_cb  = uvc_stream_event_cb,
            .frame_cb  = uvc_frame_cb,
            .user_ctx  = nullptr,
            .usb = {
                .dev_addr         = info.dev_addr,
                .vid              = UVC_HOST_ANY_VID,
                .pid              = UVC_HOST_ANY_PID,
                .uvc_stream_index = info.stream_index,
            },
            .vs_format = fmt,
            .advanced = {
                .number_of_frame_buffers = NUM_FRAME_BUFFERS,
                .frame_size              = frame_size,   // 0 なら交渉した dwMaxVideoFrameSize
                .frame_heap_caps         = MALLOC_CAP_SPIRAM,
                .number_of_urbs          = 4,
                .urb_size                = 16 * 1024,
                .user_frame_buffers      = nullptr,
            },
        };

        ESP_LOGI(TAG, "Opening stream...");
        esp_err_t err = uvc_host_stream_open(&stream_config, pdMS_TO_TICKS(5000), &s_stream);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uvc_host_stream_open failed: %s", esp_err_to_name(err));
            show_message("Camera open failed");
            continue;
        }

        M5.Display.fillScreen(TFT_BLACK);
        xQueueReset(s_frame_q);
        xSemaphoreTake(s_disconnect_sem, 0);   // 古い通知を捨てる
        s_stream_active = true;

        err = uvc_host_stream_start(s_stream);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uvc_host_stream_start failed: %s", esp_err_to_name(err));
            s_stream_active = false;
            uvc_host_stream_close(s_stream);
            s_stream = nullptr;
            show_message("Camera start failed");
            continue;
        }
        ESP_LOGI(TAG, "Streaming");

        // 切断待ち
        xSemaphoreTake(s_disconnect_sem, portMAX_DELAY);

        // 処理中のフレームが終わるのを待ってからクローズ
        s_stream_active = false;
        xSemaphoreTake(s_proc_mutex, portMAX_DELAY);
        xQueueReset(s_frame_q);
        uvc_host_stream_close(s_stream);
        s_stream = nullptr;
        xSemaphoreGive(s_proc_mutex);

        show_message("Connect a USB camera");
    }
}
