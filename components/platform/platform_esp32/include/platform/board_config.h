// components/platform_esp32/include/platform/board_config.h
#pragma once
#include <stdint.h>
#include "sdkconfig.h"
#if defined(__has_include)
#  if __has_include("driver/spi_master.h")
#    include "driver/spi_master.h"
#  else
typedef int spi_host_device_t;
#    define SPI2_HOST 1
#  endif
#else
#  include "driver/spi_master.h"
#endif

// ===== 板卡自动检测 =====
// 根据 CONFIG_IDF_TARGET 自动选择板卡配置
#if CONFIG_IDF_TARGET_ESP32S3
#  define BOARD_M5STACK_STOPWATCH 1
#  define BOARD_AI_PASSPORT       0
#elif CONFIG_IDF_TARGET_ESP32C3
#  define BOARD_M5STACK_STOPWATCH 0
#  define BOARD_AI_PASSPORT       1
#else
#  error "Unsupported target chip. Only ESP32-C3 and ESP32-S3 are supported."
#endif

// ===== 外设编译开关(1=包含该外设 / 0=不包含)=====
// 任意组合都应能编译通过。关闭某外设时:对应适配器源文件整段编译为空,
// 且组装层(app.c)不再创建/注入该外设。
#define PERIPH_DISPLAY  1   // ST7789P3 显示 + LVGL + 表现层
#define PERIPH_AUDIO    1   // ES8311 音频
#define PERIPH_BUTTON   1   // 按钮(接入 LVGL 导航:上/下/确定)
#define PERIPH_BLE      1   // BLE 档案卡控制
#define PERIPH_BATTERY  1   // 电池电量检测(CW2017 电量计,I2C)。总线上无芯片时显示未知电量

// Dedicated XiaoZhi edition: cold boot and deep wake enter the existing
// Wi-Fi/activation flow, without decoding the hidden profile avatar.
#define PASSPORT_XIAOZHI_EDITION 1

// ===== 屏幕整体圆角(适配外壳圆角开窗)=====
// 所有屏幕统一加此圆角 + 裁剪,圆角外露 4 角显示为黑边。改这一个值即可调整圆角半径。
// 单位:像素。0=不圆角(方屏)。典型 12~30,视外壳而定。
//
// ⚠ 内存代价(真机实测对照,半径 30 vs 0):整屏 clip_corner 需把子树先渲染到 layer buffer
//   再套遮罩 —— LVGL 池峰值 建界面时 +17KB、稳态 +9KB(35.4KB vs 26.9KB)。
//   故改动此项后必须重看日志的 lv_peak,并相应调 CONFIG_LV_MEM_SIZE_KILOBYTES
//   (见 sdkconfig.defaults)。
#if BOARD_M5STACK_STOPWATCH
// StopWatch: 圆形屏幕,使用完整圆形遮罩而非简单圆角
#define SCREEN_CORNER_RADIUS  0  // 圆形屏幕不使用圆角,通过 LVGL 圆形遮罩实现
#define SCREEN_IS_ROUND       1
#define DISPLAY_ROUND_EDGE_INSET  56  // 顶部/底部弧形安全区
#define DISPLAY_STATUS_BAR_TOP_OFF  56  // 状态文字:上半部分,图标下方
#define DISPLAY_CHAT_BAR_BOTTOM_OFF  66  // 字幕:从底部弧形抬升
#else
// AI PASSPORT: 方屏带圆角
#define SCREEN_CORNER_RADIUS  30
#define SCREEN_IS_ROUND       0
#endif

// ===== 电池电量检测 =====
// AI PASSPORT: CW2017 电量计 @0x63 (I2C)
// StopWatch: M5PM1 PMIC @0x6E (I2C) 提供电压读数,软件估算电量
#define BATT_SAMPLE_MS    500     // 采样周期(ms)
#define BATT_BOOT_RETRY_MS  50     // 开机快速首读间隔
#define BATT_BOOT_TIMEOUT_MS  500  // 首读短时重试窗口
#if BOARD_AI_PASSPORT
// CW2017 电量计配置
#define CW2017_WRITE_PROFILE  1   // 1=按选定 profile 写芯片(默认);0=用芯片自带 Li-Poly profile
#define CW2017_PROFILE_SEL    1   // 选用 CW2017_PROFILES[] 里第几个 profile(1=4.2v_520mah)
#endif
// ===== 调试/测试开关(平时都置 0)=====
// 麦克风直通扬声器:置 1 后开机即把麦克风采集实时输出到扬声器(对着麦说话能听到自己),
// 用于排查 ES8311 输入/输出通路是否正常。
// 注意:该模式会独占音频且不返回,开启期间乐谱播放不可用(下发乐谱会回"存储失败"状态)。
#define AUDIO_MIC_PASSTHROUGH_TEST  0

// 游戏固定分调试:置 0 关闭;置 >0 时每局结束都按该固定分记账(如 100),
// 无视实际玩得多少。用于快速验证"积分累计 / BLE 读 …0013 / 达阈值给互动"整条同步链路,
// 不必真去玩到某个分数。仅影响 app.c 的每局结算(on_game_result),游戏本身照常玩。
#define GAME_DEBUG_FIXED_SCORE  0

// I2C 总线扫描:置 1 后在 ES8311 初始化前扫描总线,打印在线设备(ES8311=0x18 / CW2017=0x63)。
// 量产默认关闭；诊断时可开启确认音频/电量芯片在线。扫描在一条【临时总线】上进行、
// 扫完即删，不污染随后 ES8311 用的正式总线(NACK 风暴会残留总线状态，故必须隔离)。
#define I2C_BUS_SCAN_DEBUG  0

// 临时深睡测试覆盖值(秒):>0 时忽略 standby_time，按该秒数进入深睡；量产前改回 0。
#define DEEP_SLEEP_TEST_SECONDS  0

// 临时调试开关:1=强制禁用自动深睡，不修改 NVS 中保存的 standby_time；量产前改回 0。
#define DEEP_SLEEP_FORCE_DISABLED  0

// ===== 音频(本地片段,ADPCM)=====
#define AUDIO_ADPCM_SR       16000   // 片段播放采样率(16k 高质;片段写 flash 后离线播放)
#define AUDIO_SPIFFS_MOUNT   "/audio"
#define AUDIO_SPIFFS_LABEL   "audio"

// BLE 设备名
#define BLE_DEVICE_NAME "TRAE-CARD"

// 按钮数量
#if BOARD_M5STACK_STOPWATCH
#define BTN_COUNT 2   // StopWatch: Button1 (G2) + Button2 (G1)
#else
#define BTN_COUNT 3   // AI PASSPORT: ADC 按键:上/下/确定 3 键,共用 GPIO0=ADC1_CH0
#endif

// 显示反色
#if BOARD_AI_PASSPORT
// ST7789P3(T200H7-C14-05)参考例程 TFT_init() 末尾无条件发送 0x21(INVON),
// 故默认开启;若换屏后颜色为负片,再改回 0。
#define LCD_INVERT_COLOR 1
#else
// StopWatch CO5300 AMOLED 不需要反色
#define LCD_INVERT_COLOR 0
#endif

typedef struct {
    // --- LCD ---
    uint16_t lcd_w, lcd_h;
    spi_host_device_t lcd_spi_host;
#if BOARD_AI_PASSPORT
    // AI PASSPORT: ST7789P3 4-line SPI
    int lcd_mosi, lcd_sclk, lcd_cs, lcd_dc, lcd_rst, lcd_bl;
#elif BOARD_M5STACK_STOPWATCH
    // StopWatch: CO5300 QSPI (使用 data0-3 而非 mosi)
    int lcd_qspi_d0, lcd_qspi_d1, lcd_qspi_d2, lcd_qspi_d3;
    int lcd_qspi_sck, lcd_qspi_cs;
    // GPIO扩展器控制的引脚 (通过 M5IOE1 间接控制,非直连)
    // lcd_rst 和 lcd_bl 由 M5IOE1 GPIO 扩展器管理
#endif
    uint32_t lcd_pclk_hz;
    
    // --- Button ---
#if BOARD_AI_PASSPORT
    // AI PASSPORT: ADC 按键,共用 GPIO0=ADC1_CH0,外部 10k 上拉;分压 0/1k/2.2k = 上/下/确定
    int btn_adc_channel;        // ADC1 通道号(GPIO0 = 0)
    int btn_wakeup_gpio;        // 深睡唤醒 GPIO
    int btn_gpio[BTN_COUNT];    // ADC 按键不使用,保留字段兼容旧代码
    uint8_t btn_active_level;   // ADC 按键不使用
#elif BOARD_M5STACK_STOPWATCH
    // StopWatch: 独立GPIO按钮
    int btn_gpio[BTN_COUNT];    // Button1=GPIO2, Button2=GPIO1
    uint8_t btn_active_level;   // 低电平有效
    int btn_wakeup_gpio;        // 深睡唤醒 GPIO
#endif

    // --- Audio (ES8311) ---
    int i2c_sda, i2c_scl;
    int i2s_mclk, i2s_bclk, i2s_ws, i2s_dout, i2s_din, pa_ctrl;
    
#if BOARD_M5STACK_STOPWATCH
    // StopWatch 特有:触摸屏中断引脚 (CST820B)
    int touch_int_gpio;
#endif
} board_config_t;

#if BOARD_AI_PASSPORT
// ===== AI PASSPORT ESP32-C3 配置 =====
// ESP32-C3 仅有 GPIO 0..21,且 GPIO 11..17 通常用于板载 SPI flash。
// lcd_rst = -1 表示复位脚不接 MCU(硬接 3.3V),改由软件复位命令初始化。
#define BOARD_CONFIG_DEFAULT() (board_config_t){              \
    /* --- LCD ST7789P3 240x320, 4-line SPI --- */           \
    .lcd_w = 240, .lcd_h = 320,                               \
    .lcd_spi_host = SPI2_HOST,                                \
    .lcd_mosi = 9, .lcd_sclk = 8, .lcd_cs = 1,               \
    .lcd_dc = 20, .lcd_rst = -1, .lcd_bl = 21,               \
    .lcd_pclk_hz = 80 * 1000 * 1000,                          \
    /* --- Button: ADC 按键,GPIO0 = ADC1_CH0 --- */          \
    .btn_adc_channel = 0,                                     \
    .btn_wakeup_gpio = 0,                                     \
    .btn_gpio = {0, 0, 0},                                    \
    .btn_active_level = 0,                                    \
    /* --- Audio ES8311 --- */                                \
    .i2c_sda = 10, .i2c_scl = 7,                              \
    .i2s_mclk = 6, .i2s_bclk = 5, .i2s_ws = 3,                \
    .i2s_dout = 2, .i2s_din = 4, .pa_ctrl = -1,               \
}

#elif BOARD_M5STACK_STOPWATCH
// ===== M5Stack StopWatch ESP32-S3 配置 =====
// 硬件规格:
//   MCU: ESP32-S3-WROOM-1 (16MB Flash, 8MB Octal PSRAM)
//   Display: CO5300 466x466 1.75" 圆形 AMOLED, QSPI 接口
//   Touch: CST820B @0x15 (I2C)
//   Audio: ES8311 @0x18 (I2C), 半双工 (麦克风和扬声器不能同时工作)
//   GPIO扩展器: M5IOE1 @0x4F (I2C)
//   PMIC: M5PM1 @0x6E (I2C)
//   USB: GPIO19/20 USB-Serial-JTAG (保留用于烧录和日志)
// 
// M5IOE1 GPIO 分配:
//   PYG8: 显示电源使能, PYG5: 显示复位, PYG4: 触摸复位
//   PYG3: 音频 Codec 电源, PYG10: 功放使能, PYG9: 马达
#define BOARD_CONFIG_DEFAULT() (board_config_t){              \
    /* --- LCD CO5300 466x466, QSPI --- */                   \
    .lcd_w = 466, .lcd_h = 466,                               \
    .lcd_spi_host = SPI2_HOST,                                \
    .lcd_qspi_d0 = 41, .lcd_qspi_d1 = 42,                    \
    .lcd_qspi_d2 = 46, .lcd_qspi_d3 = 45,                    \
    .lcd_qspi_sck = 40, .lcd_qspi_cs = 39,                   \
    .lcd_pclk_hz = 40 * 1000 * 1000,                          \
    /* 注: lcd_rst 和 lcd_bl 由 M5IOE1 扩展器控制,非直连 */    \
    /* --- Button: GPIO 独立按钮 --- */                      \
    .btn_gpio = {2, 1},  /* Button1=GPIO2, Button2=GPIO1 */  \
    .btn_active_level = 0,  /* 低电平有效 */                 \
    .btn_wakeup_gpio = 2,   /* Button1 唤醒 */               \
    /* --- Audio ES8311 (半双工) --- */                      \
    .i2c_sda = 47, .i2c_scl = 48,                             \
    .i2s_mclk = 18, .i2s_bclk = 17, .i2s_ws = 15,            \
    .i2s_dout = 16, .i2s_din = 21, .pa_ctrl = -1,            \
    /* 注: pa_ctrl 由 M5IOE1 扩展器控制 (PYG10) */           \
    /* --- Touch --- */                                       \
    .touch_int_gpio = 13,  /* CST820B 中断引脚 */            \
}

#else
#error "No board configuration defined"
#endif
