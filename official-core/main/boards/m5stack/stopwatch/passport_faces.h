#pragma once
#include <lvgl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PASSPORT_FACE_IDLE = 0,
    PASSPORT_FACE_BLINK,
    PASSPORT_FACE_LISTEN,
    PASSPORT_FACE_THINK,
    PASSPORT_FACE_TALK,
    PASSPORT_FACE_HAPPY,
    PASSPORT_FACE_COUNT
};

const lv_image_dsc_t *passport_face(int index);
size_t passport_face_count(void);

#ifdef __cplusplus
}
#endif
