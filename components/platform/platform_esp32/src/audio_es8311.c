// components/platform_esp32/src/audio_es8311.c
#include "platform/board_config.h"
#if PERIPH_AUDIO
#include "hal/hal_audio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"   // audio_codec_new_i2c_ctrl / audio_codec_new_i2s_data
#include "es8311_codec.h"             // es8311_codec_new / es8311_codec_cfg_t
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#if BOARD_M5STACK_STOPWATCH
#include "platform/stopwatch_hardware.h"
#endif

static const char *TAG = "audio_es8311";

// cur_*:当前 codec 已打开的格式。esp_codec_dev_open 在"已打开"时会直接返回 OK 且
// **不重新配置采样率**(见 esp_codec_dev.c 的 "Input already open" 分支),所以换格式
// 必须先 close。记录当前格式以便:同格式复用(不反复切换,免咔哒声)、异格式才重开。
typedef struct {
    esp_codec_dev_handle_t dev;
    const audio_codec_ctrl_if_t *ctrl_if; // 深睡时绕过 dev 打开状态，强制写 ES8311
    i2s_chan_handle_t tx, rx;   // 留存通道，休眠时停时钟；DMA存储跨格式切换保留
    int i2s_pins[5];            // MCLK/BCLK/WS/DOUT/DIN，关时钟后切为高阻
    int i2c_pins[2];            // SDA/SCL：CW2017 先休眠后由音频收尾释放共享总线
    uint32_t cur_hz;
    uint8_t  cur_bits, cur_ch;
    uint8_t  requested_volume;
    bool     opened;
    bool     i2s_needs_enable; // close 后时钟已停，只能由成功的 open 恢复
    bool     echo_capture;    // temporary ADC + DAC reference stereo capture
    bool     out_muted;        // HAL 请求；失败清理的临时静音不能覆盖它
    bool     i2c_bus_is_shared; // StopWatch: I2C bus shared with hardware (don't delete)
    // Aligned 32-bit ISR counter: one writer, task only reads snapshots.
    volatile uint32_t rx_overflows;
    uint32_t echo_overflows_seen;
    atomic_uint meter_enabled, audible_tail;
} es_ctx_t;

// IDF supplies the stereo DMA block that has just reached the DAC, before
// auto-clear. Sample one slot in eight frames; no clock, queue or UI in the ISR.
static bool IRAM_ATTR es_tx_sent(i2s_chan_handle_t channel, i2s_event_data_t *event, void *user) {
    (void)channel;
    es_ctx_t *c = user;
    if (!atomic_load_explicit(&c->meter_enabled, memory_order_relaxed)) return false;
    const int16_t *pcm = event->dma_buf;
    unsigned voiced = 0;
    for (size_t i = 0; pcm && i < event->size / sizeof(*pcm); i += 16) {
        if ((pcm[i] > 120 || pcm[i] < -120) && ++voiced == 2) break;
    }
    unsigned tail = atomic_load_explicit(&c->audible_tail, memory_order_relaxed);
    atomic_store_explicit(&c->audible_tail, voiced >= 2 ? 7 : (tail ? tail - 1 : 0), memory_order_relaxed);
    return false;
}
static bool es_output_active(hal_audio_t *self) {
    es_ctx_t *c = self->impl;
    return atomic_load_explicit(&c->meter_enabled, memory_order_relaxed) &&
           atomic_load_explicit(&c->audible_tail, memory_order_relaxed);
}
static void es_meter_enable(es_ctx_t *c, bool enabled) {
    atomic_store_explicit(&c->meter_enabled, enabled, memory_order_relaxed);
    if (!enabled) atomic_store_explicit(&c->audible_tail, 0, memory_order_relaxed);
}
static bool IRAM_ATTR es_rx_overflow(i2s_chan_handle_t channel,
                                   i2s_event_data_t *event, void *user) {
    (void)channel; (void)event;
    es_ctx_t *c = user;
    ++c->rx_overflows;
    return false; // no allocation, logging or task wake-up in the ISR
}

typedef struct {
    uint8_t reg;
    uint8_t value;
} es_reg_value_t;

// Everest ES8311 手册的 power-down 序列，与官方 esp_codec_dev suspend 一致。
// REG45 bit0 改为 1，额外关闭 BCLK/LRCK 内部上拉，避免板级下拉泄漏。
static const es_reg_value_t ES8311_SUSPEND_SEQ[] = {
    {0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02},
    {0x14, 0x00}, {0x0D, 0xFA}, {0x15, 0x00}, {0x02, 0x10},
    {0x00, 0x00}, {0x00, 0x1F}, {0x01, 0x30}, {0x01, 0x00},
    {0x45, 0x01}, {0x0D, 0xFC}, {0x02, 0x00},
};

static const es_reg_value_t ES8311_SUSPEND_CHECK[] = {
    {0x00, 0x1F}, {0x01, 0x00}, {0x0D, 0xFC},
    {0x0E, 0xFF}, {0x12, 0x02}, {0x45, 0x01},
};

static int es_ctrl_write(const audio_codec_ctrl_if_t *ctrl, uint8_t reg, uint8_t value) {
    return ctrl->write_reg(ctrl, reg, 1, &value, 1);
}

static int es_ctrl_read(const audio_codec_ctrl_if_t *ctrl, uint8_t reg, uint8_t *value) {
    *value = 0;
    return ctrl->read_reg(ctrl, reg, 1, value, 1);
}

static bool es_force_suspend_once(es_ctx_t *c) {
    for (size_t i = 0; i < sizeof(ES8311_SUSPEND_SEQ) / sizeof(ES8311_SUSPEND_SEQ[0]); i++) {
        const es_reg_value_t *rv = &ES8311_SUSPEND_SEQ[i];
        int r = es_ctrl_write(c->ctrl_if, rv->reg, rv->value);
        if (r != 0) {
            ESP_LOGE(TAG, "ES8311 休眠写失败 reg=0x%02X val=0x%02X ret=%d",
                     rv->reg, rv->value, r);
            return false;
        }
    }

    for (size_t i = 0; i < sizeof(ES8311_SUSPEND_CHECK) / sizeof(ES8311_SUSPEND_CHECK[0]); i++) {
        const es_reg_value_t *rv = &ES8311_SUSPEND_CHECK[i];
        uint8_t actual = 0;
        int r = es_ctrl_read(c->ctrl_if, rv->reg, &actual);
        // ES8311 datasheet rev10 p18 defines only REG0E bits 6:2.
        // The prescribed 0xFF write reads back 0x7F on this board; undocumented
        // bits must not trigger a second power-down or hide a real bit failure.
        uint8_t mask = rv->reg == 0x0E ? 0x7C : 0xFF;
        if (r != 0 || (actual & mask) != (rv->value & mask)) {
            ESP_LOGE(TAG, "ES8311 休眠校验失败 reg=0x%02X expect=0x%02X actual=0x%02X ret=%d",
                     rv->reg, rv->value, actual, r);
            return false;
        }
    }
    return true;
}

static void es_release_i2s_pins(es_ctx_t *c) {
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(c->i2s_pins) / sizeof(c->i2s_pins[0]); i++) {
        if (GPIO_IS_VALID_GPIO(c->i2s_pins[i])) mask |= 1ULL << c->i2s_pins[i];
    }
    if (!mask) return;
    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t r = gpio_config(&io);
    if (r != ESP_OK) ESP_LOGW(TAG, "I2S 引脚切换高阻失败: %s", esp_err_to_name(r));
}

static void es_release_i2c_pins(es_ctx_t *c) {
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(c->i2c_pins) / sizeof(c->i2c_pins[0]); i++) {
        if (GPIO_IS_VALID_GPIO(c->i2c_pins[i])) mask |= 1ULL << c->i2c_pins[i];
    }
    if (!mask) return;
    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t r = gpio_config(&io);
    if (r != ESP_OK) ESP_LOGW(TAG, "I2C 引脚释放高阻失败: %s", esp_err_to_name(r));
}

static int es_write(hal_audio_t *self, const void *pcm, size_t n, size_t *w) {
    es_ctx_t *c = (es_ctx_t *)self->impl;
    if (w) *w = 0;
    if (!c->dev || !pcm || n % sizeof(int16_t)) return -1;
    if (!c->echo_capture && (c->cur_bits != 16 || c->cur_ch != 1)) {
        int r = esp_codec_dev_write(c->dev, (void *)pcm, n);
        if (w) *w = r == 0 ? n : 0;
        return r;
    }
    /* Feed the same mono sample to both I2S slots: the speaker and the
     * ES8311 DACR reference must see identical digital playback data. */
    const int16_t *in = pcm;
    int16_t stereo[HAL_AUDIO_DUPLEX_FRAMES * 2];
    size_t samples = n / sizeof(int16_t), done = 0;
    while (done < samples) {
        size_t count = samples - done;
        if (count > HAL_AUDIO_DUPLEX_FRAMES) count = HAL_AUDIO_DUPLEX_FRAMES;
        for (size_t i = 0; i < count; ++i)
            stereo[i * 2] = stereo[i * 2 + 1] = in[done + i];
        int r = esp_codec_dev_write(c->dev, stereo, count * 2 * sizeof(int16_t));
        if (r) return r;
        done += count;
        if (w) *w = done * sizeof(int16_t);
    }
    return 0;
}
static int es_read(hal_audio_t *self, void *pcm, size_t n, size_t *got) {
    es_ctx_t *c = (es_ctx_t *)self->impl;
    if (got) *got = 0;
    if (!c->dev || !pcm || n % sizeof(int16_t)) return -1;
    if (c->cur_bits != 16 || c->cur_ch != 1) {
        int r = esp_codec_dev_read(c->dev, pcm, n);
        if (got) *got = r == 0 ? n : 0;
        return r;
    }
    // Keep the physical bus stereo even for mono clients, retaining DMA
    // storage across listen/play transitions. ADC is the first I2S slot.
    int16_t stereo[HAL_AUDIO_DUPLEX_FRAMES * 2], *out = pcm;
    size_t samples = n / sizeof(int16_t), done = 0;
    while (done < samples) {
        size_t count = samples - done;
        if (count > HAL_AUDIO_DUPLEX_FRAMES) count = HAL_AUDIO_DUPLEX_FRAMES;
        int r = esp_codec_dev_read(c->dev, stereo, count * 2 * sizeof(int16_t));
        if (r) { memset(stereo, 0, sizeof stereo); return r; }
        for (size_t i = 0; i < count; ++i) out[done + i] = stereo[2 * i];
        done += count;
        if (got) *got = done * sizeof(int16_t);
    }
    memset(stereo, 0, sizeof stereo);
    return 0;
}
static int es_apply_volume(es_ctx_t *c) {
    if (!c->dev) return -1;
    // The calibrated curve maps95 to -10.1dB, matching the old80 (-10dB)
    // acoustic echo limit. Preserve that tested loudness in duplex mode.
    unsigned volume = c->requested_volume;
    if (c->echo_capture && volume > 95) volume = 95;
    int result = esp_codec_dev_set_out_vol(c->dev, volume);
    es_meter_enable(c, !result && c->opened && !c->out_muted && volume && c->cur_bits == 16);
    return result;
}
static void es_volume(hal_audio_t *self, uint8_t p) {
    es_ctx_t *c = self->impl;
    c->requested_volume = p > 100 ? 100 : p;
    (void)es_apply_volume(c);
}
static int es_out_mute(hal_audio_t *self, bool mute) {
    es_ctx_t *c = (es_ctx_t*)self->impl;
    esp_codec_dev_handle_t d = c->dev;
    if (!d) return -1;
    c->out_muted = mute;
    int result = esp_codec_dev_set_out_mute(d, mute);
    es_meter_enable(c, !result && !mute && c->opened && c->requested_volume && c->cur_bits == 16);
    return result;
}

static void es_close(es_ctx_t *c) {
    es_meter_enable(c, false);
    // Mute before disabling clocks; keep out_muted as the caller's preference.
    (void)esp_codec_dev_set_out_mute(c->dev, true);
    
#if BOARD_M5STACK_STOPWATCH
    // Half-duplex: disable PA when closing codec
    stopwatch_speaker_enable(false);
#endif
    
    // open may set SDK input/output_opened before set_fs/enable fails. Always
    // close, even when our own opened flag is false, to remove that partial state.
    (void)esp_codec_dev_close(c->dev);
    c->opened = false;
    c->cur_hz = 0; c->cur_bits = 0; c->cur_ch = 0;
    c->i2s_needs_enable = true;
}

static int es_output_curve(esp_codec_dev_handle_t dev) {
    // The ES8311 driver compensates5V/3.3V by +3.61dB. Its default curve
    // therefore applies +1dB at95 and +3.5dB at100 to PCM that may already
    // reach full scale. Keep at least4.4dB of DAC headroom at the maximum.
    // The SDK copies these two points into its existing two-point allocation.
    esp_codec_dev_vol_map_t points[] = {{.vol=0, .db_value=-50.0f},
                                      {.vol=100, .db_value=-8.0f}};
    esp_codec_dev_vol_curve_t curve = {.count=2, .vol_map=points};
    return esp_codec_dev_set_vol_curve(dev, &curve);
}

static int es_shutdown(hal_audio_t *self) {
    es_ctx_t *c = (es_ctx_t *)self->impl;
    if (!c || !c->ctrl_if) return -1;
    es_meter_enable(c, false);

    // dev close 仅在播放/录音打开过时才会调 codec suspend；没播放过时会直接返回。
    // 因此先让上层数据通道停止，再不依赖 dev 状态强制写完整休眠序列。
    bool was_open = c->opened;
    if (c->dev) (void)esp_codec_dev_close(c->dev);

    bool suspended = false;
    for (int attempt = 1; attempt <= 2 && !suspended; attempt++) {
        suspended = es_force_suspend_once(c);
        if (!suspended && attempt == 1) {
            ESP_LOGW(TAG, "ES8311 休眠未确认，5ms 后重试");
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    // 未打开过音频时，初始化为规避首次 open 误报而开启的 I2S 仍在运行。
    // 完成寄存器 power-down 后再停时钟，符合 ES8311 手册推荐顺序。
    if (!was_open && !c->i2s_needs_enable) {
        if (c->tx) (void)i2s_channel_disable(c->tx);
        if (c->rx) (void)i2s_channel_disable(c->rx);
    }
    es_release_i2s_pins(c);
    // app 保证 CW2017 先于 ES8311 休眠，此刻共享 I2C 已无传输，可去掉 MCU 内部上拉。
    es_release_i2c_pins(c);

    c->opened = false;
    c->cur_hz = 0;
    c->cur_bits = 0;
    c->cur_ch = 0;
    c->i2s_needs_enable = true;
    if (suspended) {
        ESP_LOGI(TAG, "ES8311 已强制休眠并通过寄存器校验，I2S 引脚已高阻");
        return 0;
    }
    ESP_LOGE(TAG, "ES8311 强制休眠失败");
    return -1;
}
// 打开 codec device 并设置输入增益。
// 关键:esp_codec_dev_open() 内部会调用 es8311 驱动的 es8311_config_sample(),
// 依据采样率与 MCLK 精确算出并写入所有时钟分频/OSR/BCLK 寄存器
// (REG01/02/03/04/05/06)以及数据格式。这里绝对不能在 open 之后再手动覆写
// 这些寄存器 —— 之前那样做会盖掉驱动算好的时钟分频,导致 ADC/DAC 采样时序
// 错乱,录音回放全是杂音。参考 folo 实现:open 之后只设增益,别碰寄存器。
static int es_rate(hal_audio_t *self, uint32_t hz, uint8_t bits, uint8_t ch) {
    es_ctx_t *c = (es_ctx_t*)self->impl;
    esp_codec_dev_handle_t d = c->dev;
    if (!d) return -1;
    // 已按同一格式打开 → 直接复用(反复 close/open 会有咔哒声且拖慢起播)。
    if (c->opened && c->cur_hz == hz && c->cur_bits == bits && c->cur_ch == ch) return 0;
    // 格式不同(如开机音 16k → 乐谱 8k):必须先 close,否则 open 见"已打开"直接返回,
    // 采样率不会被改,数据会以上一个速率播出(音调/速度错一倍)。
    if (c->opened) {
        es_close(c);                    // close 会把 I2S 通道 disable 回 READY
    }
    // A failed format change may have freed DMA descriptors. Only open() may
    // restart channels, after successful reconfiguration; never pre-enable a
    // stopped channel merely to suppress an already-disabled driver message.
    c->i2s_needs_enable = false;
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        // All shipped audio uses16bit mono. Keep both physical slots allocated
        // from initialization; es_read/es_write adapt the mono client format.
        // Networking can no longer consume DMA SRAM temporarily freed in LISTEN.
        .channel = bits == 16 && ch == 1 ? 2 : ch,
        .channel_mask = ch == 2 || (bits == 16 && ch == 1) ? 3 : ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = hz,
        .mclk_multiple = 0,   // 0 → 驱动按默认 256×fs 取 MCLK
    };
    int r = esp_codec_dev_open(d, &fs);
    if (r == 0) {
        // 驻极体麦模拟 PGA 增益。folo 参考取 30dB,电容麦常用起点。
        esp_codec_dev_set_in_gain(d, 30.0f);
        // A failed open was muted for safety; restore the caller's setting only
        // after a complete reopen, preserving capture mute across retries.
        int mute_result = esp_codec_dev_set_out_mute(d, c->out_muted);
        c->opened = true;
        c->cur_hz = hz; c->cur_bits = bits; c->cur_ch = ch;
        
#if BOARD_M5STACK_STOPWATCH
        // Half-duplex: enable PA after codec open (controlled via M5IOE1)
        stopwatch_speaker_enable(true);
#endif
        
        es_meter_enable(c, !mute_result && !c->out_muted && c->requested_volume && bits == 16);
        ESP_LOGD(TAG, "codec 打开 %uHz/%ubit/%uch", (unsigned)hz, bits, ch);
    } else {
        // ES8311 enable itself can partially start/unmute before failing, so
        // close alone is insufficient: its enable(false) may short-circuit.
        (void)esp_codec_dev_set_out_mute(d, true);
        es_close(c);
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %d", r);
    }
    return r;
}

/* Mode is private to the exclusive voice audio owner. A failed transition
 * mutes and closes the device, so the next ordinary acquire reopens mono. */
static int es_echo_capture(hal_audio_t *self, bool enable) {
    es_ctx_t *c = (es_ctx_t *)self->impl;
    if (!c || !c->dev) return -1;
    if (enable && c->echo_capture) return 0;
    int r = 0;
    if (enable && c->opened) {
        r = es_rate(self, c->cur_hz, 16, 2);
        if (!r) r = es_ctrl_write(c->ctrl_if, 0x44, 0x58);
        if (!r) {
            c->echo_capture = true;
            r = es_apply_volume(c);
            if (!r) {
                c->echo_overflows_seen = c->rx_overflows;
                return 0;
            }
        }
    } else if (enable) r = -1;
    (void)esp_codec_dev_set_out_mute(c->dev, true);
    int restore = es_ctrl_write(c->ctrl_if, 0x44, 0x08);
    c->echo_capture = false;
    int volume = es_apply_volume(c);
    es_close(c);
    return r ? r : restore ? restore : volume;
}
static int es_read_echo(hal_audio_t *self, int16_t *stereo, size_t frames, size_t *got) {
    es_ctx_t *c = (es_ctx_t *)self->impl;
    if (got) *got = 0;
    if (!c || !c->dev || !c->echo_capture || !stereo || !frames || frames > 1440) return -1;
    int r = esp_codec_dev_read(c->dev, stereo, frames * 2 * sizeof(int16_t));
    if (got) *got = r == 0 ? frames : 0;
    if (!r && c->echo_overflows_seen != c->rx_overflows) {
        c->echo_overflows_seen = c->rx_overflows;
        return HAL_AUDIO_ECHO_GAP;
    }
    return r;
}

static const hal_audio_api_t API = {
    .write = es_write, .read = es_read, .set_volume = es_volume, .set_sample_rate = es_rate,
    .set_out_mute = es_out_mute, .shutdown = es_shutdown,
    .set_echo_capture = es_echo_capture, .read_echo = es_read_echo, .output_active = es_output_active,
};

// 全双工 I2S:同端口一 tx 一 rx,共用 MCLK/BCLK/WS,DOUT 播放、DIN 录音。
// 配置对齐 folo 参考:主模式、MCLK=256×fs、16bit 立体声 Philips 时序。
// 只 init_std_mode 配好引脚/slot,不在此 enable —— esp_codec_dev 在 open 时
// 会自行 reconfig_clock + enable 通道(见 audio_codec_data_i2s.c)。
static int i2s_full_duplex_init(const board_config_t *cfg, uint32_t sample_rate,
                                i2s_chan_handle_t *tx, i2s_chan_handle_t *rx,
                                es_ctx_t *owner) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 7,
        // IDF can abandon a partially consumed block when the queue is nearly
        // full. Match every voice transfer to a complete block, including the
        // end of each60ms packet. Seven blocks cover52.5ms at16k /35ms at24k;
        // stereo duplex sample storage is6720B (+576B over six128-frame blocks).
        .dma_frame_num = HAL_AUDIO_DUPLEX_FRAMES,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t e = i2s_new_channel(&chan_cfg, tx, rx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel 失败: %d", e); return -1; }

    // 初始速率仅用于建通道;实际速率在 esp_codec_dev_open 时按需重配。
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = cfg->i2s_mclk, .bclk = cfg->i2s_bclk, .ws = cfg->i2s_ws,
            .dout = cfg->i2s_dout, .din = cfg->i2s_din,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if ((e = i2s_channel_init_std_mode(*tx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init tx 失败: %d", e); return -1;
    }
    if ((e = i2s_channel_init_std_mode(*rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init rx 失败: %d", e); return -1;
    }
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = es_rx_overflow};
    const i2s_event_callbacks_t output = {.on_sent = es_tx_sent};
    if (i2s_channel_register_event_callback(*tx, &output, owner) != ESP_OK) return -1;
    if ((e = i2s_channel_register_event_callback(*rx, &callbacks, owner)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s RX continuity callback 失败: %d", e); return -1;
    }
    // esp_codec_dev_open 内部重配前会先 i2s_channel_disable(见 audio_codec_data_i2s.c),
    // 而 disable 要求通道处于 RUNNING;首次 open 时通道刚 init(READY)未使能 → 驱动打印
    // "i2s_channel_disable: the channel has not been enabled yet"(无害,仅首次)。
    // 这里 init 后先 enable 一次,让首次 open 的 disable 合法 → 消掉该 ESP_LOGE。
    // 此刻 PA 未开、codec 未配,I2S 空转不出声(auto_clear 送零),无副作用。
    i2s_channel_enable(*tx);
    i2s_channel_enable(*rx);
    return 0;
}

#if I2C_BUS_SCAN_DEBUG
// I2C 总线扫描:逐个 7 位地址探测,打印所有 ACK 的设备,便于确认 ES8311
// 是否在线、接线是否正常。注意 ES8311_CODEC_DEFAULT_ADDR 是 8 位形式(0x30),
// 这里比对的是 7 位地址,故右移一位 → 0x18。
// 仅用于开机诊断,默认不编译(见 board_config.h 的 I2C_BUS_SCAN_DEBUG)。
static void i2c_bus_scan(i2c_master_bus_handle_t bus) {
    ESP_LOGI(TAG, "I2C 扫描开始(SDA/SCL 上的设备):");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  发现设备 @ 0x%02X%s", addr,
                     addr == (ES8311_CODEC_DEFAULT_ADDR >> 1) ? "  <- ES8311" : "");
            found++;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "  未发现任何 I2C 设备(检查接线/上拉/供电)");
    else            ESP_LOGI(TAG, "I2C 扫描完成,共 %d 个设备", found);
}
#endif // I2C_BUS_SCAN_DEBUG

// 工厂:i2c_master → es8311_codec_new → i2s_std(全双工) → esp_codec_dev_new。
// 任一步失败时 c->dev 留 NULL,HAL 读写会返回 -1(不崩)。
hal_audio_t *platform_create_audio(const board_config_t *cfg) {
    es_ctx_t *c = calloc(1, sizeof(*c));
    if (c) c->out_muted = true;

    // 1) I2C master 总线(ES8311 控制口)
    // StopWatch: I2C bus shared with M5IOE1/M5PM1/CST820B, already created in app.c
    // AI PASSPORT: Dedicated audio I2C bus, create here
    i2c_master_bus_handle_t i2c_bus = NULL;
    bool i2c_bus_is_shared = false;
    
#if BOARD_M5STACK_STOPWATCH
    // Use shared I2C bus created in app.c for hardware init
    extern i2c_master_bus_handle_t g_stopwatch_i2c_bus;
    if (g_stopwatch_i2c_bus) {
        i2c_bus = g_stopwatch_i2c_bus;
        i2c_bus_is_shared = true;
        ESP_LOGI(TAG, "Using shared I2C bus for ES8311 (StopWatch)");
    } else {
        ESP_LOGE(TAG, "Shared I2C bus not available (g_stopwatch_i2c_bus is NULL)");
        goto done;
    }
#else
    // AI PASSPORT: Create dedicated I2C bus for audio
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = cfg->i2c_sda,
        .scl_io_num = cfg->i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
#if I2C_BUS_SCAN_DEBUG
    // 加载 ES8311 驱动前扫描总线,确认 ES8311(0x18)/CW2017(0x63) 等是否在线(两者共用此总线)。
    // 用一条临时总线扫完即删:扫描对上百个空地址 probe 的 NACK 会污染 IDF i2c_master 的总线
    // 状态,若在正式总线上扫会导致随后 ES8311 读写失败;隔离到临时总线、扫完删除即可避开。
    {
        i2c_master_bus_handle_t scan_bus = NULL;
        if (i2c_new_master_bus(&i2c_cfg, &scan_bus) == ESP_OK) {
            i2c_bus_scan(scan_bus);
            i2c_del_master_bus(scan_bus);
        }
    }
#endif

    esp_err_t e = i2c_new_master_bus(&i2c_cfg, &i2c_bus);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2c_new_master_bus 失败: %d", e); goto done; }
#endif

    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    });
    if (!ctrl_if) { ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl 失败"); goto done; }
    c->ctrl_if = ctrl_if;
    c->i2c_bus_is_shared = i2c_bus_is_shared;
    c->i2c_pins[0] = cfg->i2c_sda;
    c->i2c_pins[1] = cfg->i2c_scl;

    // 2) I2S 全双工数据口
    i2s_chan_handle_t tx = NULL, rx = NULL;
    if (i2s_full_duplex_init(cfg, 16000, &tx, &rx, c) != 0) goto done;
    c->tx = tx; c->rx = rx;   // 留存供 es_rate 换采样率时补 enable
    c->i2s_pins[0] = cfg->i2s_mclk;
    c->i2s_pins[1] = cfg->i2s_bclk;
    c->i2s_pins[2] = cfg->i2s_ws;
    c->i2s_pins[3] = cfg->i2s_dout;
    c->i2s_pins[4] = cfg->i2s_din;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = rx,
    });
    if (!data_if) { ESP_LOGE(TAG, "audio_codec_new_i2s_data 失败"); goto done; }

    // 3) ES8311 codec 接口:ADC+DAC,外部 MCLK,PA 由 pa_ctrl 使能。
    //    hw_gain 对齐 folo 参考(PA 5V / DAC 3.3V),用于音量曲线换算。
    const audio_codec_if_t *codec_if = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if     = ctrl_if,
        .gpio_if     = audio_codec_new_gpio(),
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin      = cfg->pa_ctrl,
        .pa_reverted = false,
        .master_mode = false,   // MCU I2S 为 master,codec 为 slave
        .use_mclk    = true,
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
        // 单声道纯麦克风录音:必须 true,否则驱动写 REG44=0x58 进入 ADCL+DACR 参考模式,
        // 单声道读到的那一路是 DAC 参考(录音时静音)→ 录音恒为 0。true → REG44=0x08 纯 ADC。
        .no_dac_ref  = true,
    });
    if (!codec_if) { ESP_LOGE(TAG, "es8311_codec_new 失败"); goto done; }

    // 4) 组合成可读写的 codec device
    c->dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    });
    if (!c->dev) ESP_LOGE(TAG, "esp_codec_dev_new 失败");
    else if (es_output_curve(c->dev) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "安全音量曲线初始化失败");
        esp_codec_dev_delete(c->dev);
        c->dev = NULL;
    } else ESP_LOGI(TAG, "ES8311 初始化完成（输出增益已校准）");

done:;
    hal_audio_t *a = calloc(1, sizeof(*a));
    a->api = &API; a->impl = c;
    return a;
}

// ================= 音频直通(麦克风→扬声器实时) =================
// 对着麦克风说话,扬声器同步放出:codec 只 open 一次,之后持续 read(ADC)→write(DAC)。
// 单声道 16k,小 static buffer 不占堆;不反复重配 codec(避免 I2C 偶发失败)。
// 进入后不返回,在自检任务里独占运行。
int platform_audio_passthrough(hal_audio_t *a) {
    if (!a) return -1;
    es_ctx_t *c = (es_ctx_t*)a->impl;
    esp_codec_dev_handle_t d = c->dev;
    if (!d) { ESP_LOGE(TAG, "直通:codec 未就绪"); return -1; }

    if (es_rate(a, 16000, 16, 1) != 0) { ESP_LOGE(TAG, "直通:open 失败"); return -2; }
    es_out_mute(a, false);
    esp_codec_dev_set_out_vol(d, 80);
    ESP_LOGI(TAG, "音频直通已启动:对着麦克风说话,扬声器实时输出");

    static int16_t buf[320];   // 20ms @16k 单声道,低延迟
    for (;;) {
        size_t got = 0, written = 0;
        if (es_read(a, buf, sizeof(buf), &got) == 0 && got == sizeof(buf))
            es_write(a, buf, sizeof(buf), &written);
    }
    return 0;   // 不会到达
}
#endif // PERIPH_AUDIO
