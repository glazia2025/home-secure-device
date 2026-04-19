/* Redirect <lvgl/lvgl.h> → the managed component's lvgl.h
 * Needed because EEZ-generated files use the <lvgl/lvgl.h> include style,
 * while the ESP-IDF managed component exposes it as plain <lvgl.h>. */
#pragma once
#include <lvgl.h>
