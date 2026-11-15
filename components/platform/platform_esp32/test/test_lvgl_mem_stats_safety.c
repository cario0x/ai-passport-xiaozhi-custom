// components/platform/platform_esp32/test/test_lvgl_mem_stats_safety.c
// Regression test for boot crash fix: platform_lvgl_mem_stats must be safe
// to call before LVGL initialization (prevents IntegerDivideByZero in
// lv_mem_monitor_core when log_heap called before display init on StopWatch).

#include "unity.h"
#include "platform/board_config.h"

// External function under test
extern void platform_lvgl_mem_stats(uint32_t *used, uint32_t *free_sz, uint32_t *max_blk);

// Test that platform_lvgl_mem_stats doesn't crash when called before LVGL init
TEST_CASE("platform_lvgl_mem_stats safe before LVGL init", "[platform][lvgl]")
{
    uint32_t used = 0xDEADBEEF;
    uint32_t free_sz = 0xDEADBEEF;
    uint32_t max_blk = 0xDEADBEEF;
    
    // This should NOT crash even if LVGL not initialized
    // (Regression: StopWatch boot called log_heap before platform_lvgl_init)
    platform_lvgl_mem_stats(&used, &free_sz, &max_blk);
    
    // When LVGL not initialized, should return zeros safely
    TEST_ASSERT_EQUAL_UINT32(0, used);
    TEST_ASSERT_EQUAL_UINT32(0, free_sz);
    TEST_ASSERT_EQUAL_UINT32(0, max_blk);
}

TEST_CASE("platform_lvgl_mem_stats NULL pointers safe", "[platform][lvgl]")
{
    // Should handle NULL pointers without crash
    platform_lvgl_mem_stats(NULL, NULL, NULL);
    
    uint32_t used = 0xDEADBEEF;
    platform_lvgl_mem_stats(&used, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, used);
    
    uint32_t free_sz = 0xDEADBEEF;
    platform_lvgl_mem_stats(NULL, &free_sz, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, free_sz);
    
    uint32_t max_blk = 0xDEADBEEF;
    platform_lvgl_mem_stats(NULL, NULL, &max_blk);
    TEST_ASSERT_EQUAL_UINT32(0, max_blk);
}
