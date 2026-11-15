// components/platform/platform_esp32/src/battery_cw2017.c —— CW2017 锂电电量计驱动(hal_battery)
// CellWise CW2017,I2C 7 位地址 0x63,与 ES8311 共用 I2C_NUM_0 总线。
// 电量计自带 FastCali,直接给 SOC 百分比(0x04 高字节即整数%),无需采样电阻/电压查表。
// 默认关闭时(PERIPH_BATTERY=0)本文件整段空编译;启用后总线上无芯片则探测失败返回 NULL,
// 上层显示电量未知,不崩。电池 profile 见 cw2017_profiles.h(可切换),写入流程见下。
// 
// AI PASSPORT 专用: StopWatch 使用 battery_m5pm1.c
#include "platform/board_config.h"
#if PERIPH_BATTERY && BOARD_AI_PASSPORT
#include "platform/platform_factory.h"
#include "hal/hal_battery.h"
#include "cw2017_profiles.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdatomic.h>

static const char *TAG = "cw2017";

#define CW_ADDR           0x63   // 7 位从地址(读 0xC7 / 写 0xC6)
#define CW_REG_VERSION    0x00   // 默认 0xA0
#define CW_REG_VCELL_H    0x02   // 14bit,V(uV)=raw*312.5
#define CW_REG_SOC_H      0x04   // 高字节=整数百分比,低字节(0x05)=1/256%
#define CW_REG_CONFIG     0x08   // 0xF0=睡眠;0x30=复位态;0x00=正常。0x30→0x00 重载 profile
#define CW_REG_SOC_ALERT  0x0B   // bit7=UPDATE_FLAG(profile 已写标记);[6:0]=SOC 报警阈值
#define CW_REG_VOLT_ID_H  0x0E   // ID 引脚 ADC(有符号 14bit),V(uV)=raw*312.5
#define CW_REG_BATINFO    0x10   // profile 区 0x10..0x5F(80 字节,CW221x/2017 家族 BATINFO)
#define CW_CONFIG_RESTART 0x30
#define CW_CONFIG_NORMAL  0x00
#define CW_CONFIG_SLEEP   0xF0
#define CW_UPDATE_FLAG    0x80
#ifndef CW2017_PROFILE_SEL
#define CW2017_PROFILE_SEL 0     // 选用哪个电池 profile(索引进 CW2017_PROFILES[])
#endif
#ifndef CW2017_WRITE_PROFILE
#define CW2017_WRITE_PROFILE 1   // 1=按选定 profile 写入芯片;0=用芯片自带 Li-Poly profile
#endif

static i2c_master_dev_handle_t s_dev;
// HAL 的 timer/AT 读取不等待彼此；shutdown 先关入口，再等待正在进行的事务结束。
// deadline/首读状态在 s_io_lock 内访问；初始化在句柄交给调用方之前完成。
static atomic_flag s_io_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_reads_enabled;
static bool s_soc_reported;

static int cw_rd(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}
static int cw_wr(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

static bool cw_begin_read(void) {
    if (!atomic_load(&s_reads_enabled)) return false;
    if (atomic_flag_test_and_set(&s_io_lock)) return false;
    if (!atomic_load(&s_reads_enabled)) {
        atomic_flag_clear(&s_io_lock);
        return false;
    }
    return true;
}

static int cw_read_config_normal(void) {
    uint8_t config = 0;
    return cw_rd(CW_REG_CONFIG, &config, 1) == 0 && config == CW_CONFIG_NORMAL ? 0 : -1;
}

// VCELL:14bit,mV = raw*312.5/1000 = raw*5/16。调用方持有 s_io_lock。
static int cw_read_mv_raw(void) {
    uint8_t b[2];
    if (cw_rd(CW_REG_VCELL_H, b, 2) != 0) return -1;
    uint32_t raw = ((uint32_t)(b[0] & 0x3F) << 8) | b[1];
    return (int)(raw * 5 / 16);
}

// 只发布芯片整数 SOC，不平滑、不按电压推算、不把真实 0% 当作无效。
// 无效读数只等待下次只读采样，不重写 profile，也不再次复位。
static int cw_read_soc(hal_battery_t *self) {
    (void)self;
    if (!cw_begin_read()) return -1;
    int result = -1;
    int mv = -1;
    uint8_t b[2];
    if (cw_read_config_normal() != 0) goto done;
    if (!s_soc_reported) {
        mv = cw_read_mv_raw();
        if (mv <= 0) goto done;  // VCELL 仍为默认零值或读失败，不能确认首次转换。
    }
    if (cw_rd(CW_REG_SOC_H, b, sizeof(b)) != 0 || b[0] > 100) goto done;
    if (!atomic_load(&s_reads_enabled)) goto done;
    result = b[0];
    if (!s_soc_reported) {
        s_soc_reported = true;
        ESP_LOGI(TAG, "首次有效 SOC: %d%%, VCELL=%d mV(芯片原始值,无软件平滑)", result, mv);
    }
done:
    atomic_flag_clear(&s_io_lock);
    return result;
}

static int cw_read_mv(hal_battery_t *self) {
    (void)self;
    if (!cw_begin_read()) return -1;
    int mv = cw_read_config_normal() == 0 ? cw_read_mv_raw() : -1;
    if (!atomic_load(&s_reads_enabled)) mv = -1;
    atomic_flag_clear(&s_io_lock);
    return mv;
}
// ID 引脚电压 mV(有符号 14bit)。仅用于开机诊断打印,不参与电量计算。
static int cw_read_id_mv(void) {
    uint8_t b[2];
    if (cw_rd(CW_REG_VOLT_ID_H, b, 2) != 0) return 0;
    int raw = (int)(((uint16_t)(b[0] & 0x3F) << 8) | b[1]);
    if (raw & 0x2000) raw -= 0x4000;           // bit13 符号位 → 补码还原
    return raw * 5 / 16;                        // *312.5uV → mV
}

static int cw_set_config(uint8_t config, unsigned delay_ms) {
    if (cw_wr(CW_REG_CONFIG, config) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    uint8_t actual = 0;
    if (cw_rd(CW_REG_CONFIG, &actual, 1) != 0) return -1;
    if (actual != config) {
        ESP_LOGE(TAG, "CONFIG 校验失败 expected=0x%02X actual=0x%02X", config, actual);
        return -1;
    }
    return 0;
}

static int cw_enter_active(void) {
    if (cw_set_config(CW_CONFIG_RESTART, 20) != 0) return -1;
    return cw_set_config(CW_CONFIG_NORMAL, 20);
}

#if CW2017_WRITE_PROFILE
static const cw2017_profile_t *active_profile(void) {
    int s = (CW2017_PROFILE_SEL >= 0 && CW2017_PROFILE_SEL < CW2017_PROFILE_COUNT)
            ? CW2017_PROFILE_SEL : 0;
    return &CW2017_PROFILES[s];
}

// 三态结果:-1=读失败;0=匹配;1=确实需要更新。总线故障绝不能触发 profile 写入/重启。
static int cw_need_update(const cw2017_profile_t *prof) {
    uint8_t alert = 0;
    if (cw_rd(CW_REG_SOC_ALERT, &alert, 1) != 0) return -1;
    if (!(alert & CW_UPDATE_FLAG)) return 1;
    bool mismatch = false;
    for (int i = 0; i < CW2017_PROFILE_LEN; i++) {
        uint8_t cur;
        if (cw_rd((uint8_t)(CW_REG_BATINFO + i), &cur, 1) != 0) return -1;
        if (cur != prof->data[i]) mismatch = true;
    }
    return mismatch ? 1 : 0;
}

// 保留既有顺序:复位态 → 写入并完整校验 80 字节 → 正常态重载 → 最后标记成功。
// 任一步失败均不发布 SOC；不通过补写成功标志来掩盖失败。
static int cw_write_profile(const cw2017_profile_t *prof) {
    ESP_LOGI(TAG, "写入电池 profile '%s'(%d 字节 → 0x10)", prof->name, CW2017_PROFILE_LEN);
    if (cw_set_config(CW_CONFIG_RESTART, 30) != 0) return -1;
    for (int i = 0; i < CW2017_PROFILE_LEN; i++) {
        if (cw_wr((uint8_t)(CW_REG_BATINFO + i), prof->data[i]) != 0) {
            ESP_LOGE(TAG, "profile 写入失败:index=%d", i);
            return -1;
        }
    }
    for (int i = 0; i < CW2017_PROFILE_LEN; i++) {
        uint8_t actual = 0;
        if (cw_rd((uint8_t)(CW_REG_BATINFO + i), &actual, 1) != 0) return -1;
        if (actual != prof->data[i]) {
            ESP_LOGE(TAG, "profile 校验失败:index=%d expected=0x%02X actual=0x%02X",
                     i, prof->data[i], actual);
            return -1;
        }
    }
    if (cw_set_config(CW_CONFIG_NORMAL, 30) != 0) return -1;
    uint8_t alert = 0;
    if (cw_rd(CW_REG_SOC_ALERT, &alert, 1) != 0) return -1;
    alert |= CW_UPDATE_FLAG;
    if (cw_wr(CW_REG_SOC_ALERT, alert) != 0) return -1;
    uint8_t actual = 0;
    if (cw_rd(CW_REG_SOC_ALERT, &actual, 1) != 0 || actual != alert) return -1;
    return 0;
}
#endif // CW2017_WRITE_PROFILE

// 上电只完成配置，不阻塞等待 SOC。正常运行且 profile 匹配时不复位、不增加窗口。
static int cw_power_on(void) {
    atomic_store(&s_reads_enabled, false);
    s_soc_reported = false;
    uint8_t ver = 0;
    if (cw_rd(CW_REG_VERSION, &ver, 1) != 0) { ESP_LOGW(TAG, "未应答(总线上无 CW2017?)"); return -1; }
    ESP_LOGI(TAG, "检测到 CW2017 VERSION=0x%02X", ver);
    uint8_t config = 0;
    if (cw_rd(CW_REG_CONFIG, &config, 1) != 0) return -1;
    bool restarted = false;
#if CW2017_WRITE_PROFILE
    const cw2017_profile_t *prof = active_profile();
    int update = cw_need_update(prof);
    if (update < 0) {
        ESP_LOGE(TAG, "读取 profile 失败,不写入/不复位电量计");
        return -1;
    }
    if (update > 0) {
        if (cw_write_profile(prof) != 0) {
            ESP_LOGE(TAG, "profile 更新失败,电量保持未知");
            return -1;
        }
        restarted = true;
    } else {
        ESP_LOGI(TAG, "profile '%s' 已匹配,不重写", prof->name);
    }
#endif
    // 更新 profile 已经启动过一次；否则只有非正常态才执行唤醒(也覆盖遗留 0x30)。
    if (!restarted && config != CW_CONFIG_NORMAL) {
        if (cw_enter_active() != 0) {
            ESP_LOGE(TAG, "唤醒 CW2017 失败,电量保持未知");
            return -1;
        }
        restarted = true;
    }
    if (restarted && config == CW_CONFIG_SLEEP)
        ESP_LOGI(TAG, "CW2017 已从休眠状态唤醒");
    ESP_LOGI(TAG, "ID 引脚电压 = %d mV(诊断用)", cw_read_id_mv());
    atomic_store(&s_reads_enabled, true);
    return 0;
}

static int cw_shutdown(hal_battery_t *self) {
    (void)self;
    if (!s_dev) return -1;
    atomic_store(&s_reads_enabled, false);
    int64_t deadline = esp_timer_get_time() + 1000000LL;
    while (atomic_flag_test_and_set(&s_io_lock)) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "CW2017 休眠等待读事务结束超时");
            return -1;
        }
        vTaskDelay(1);
    }
    for (int attempt = 1; attempt <= 2; attempt++) {
        uint8_t config = 0;
        int wr = cw_wr(CW_REG_CONFIG, CW_CONFIG_SLEEP);
        vTaskDelay(pdMS_TO_TICKS(5));
        int rd = cw_rd(CW_REG_CONFIG, &config, 1);
        if (wr == 0 && rd == 0 && config == CW_CONFIG_SLEEP) {
            ESP_LOGI(TAG, "CW2017 已进入休眠并通过寄存器校验");
            atomic_flag_clear(&s_io_lock);
            return 0;
        }
        ESP_LOGW(TAG, "CW2017 休眠校验失败 attempt=%d wr=%d rd=%d config=0x%02X",
                 attempt, wr, rd, config);
    }
    ESP_LOGE(TAG, "CW2017 连续两次未能进入休眠");
    atomic_flag_clear(&s_io_lock);
    return -1;
}

static const hal_battery_api_t API = {
    .read_soc = cw_read_soc, .read_mv = cw_read_mv, .shutdown = cw_shutdown,
};
static hal_battery_t s_handle = { .api = &API, .impl = NULL };

hal_battery_t *platform_create_battery(const board_config_t *cfg) {
    // 复用与 ES8311 同一条 I2C 总线(I2C_NUM_0);若音频未启用/总线未建,则自建一条。
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK || !bus) {
        i2c_master_bus_config_t bc = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = cfg->i2c_sda,
            .scl_io_num = cfg->i2c_scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
            ESP_LOGW(TAG, "I2C 总线不可用,电量禁用");
            return NULL;
        }
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CW_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "挂载 CW2017 设备失败,电量禁用");
        return NULL;
    }
    if (cw_power_on() != 0) {
        i2c_master_bus_rm_device(s_dev); s_dev = NULL;
        return NULL;
    }
    return &s_handle;
}

#endif // PERIPH_BATTERY && BOARD_AI_PASSPORT
