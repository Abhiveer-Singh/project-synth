#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include "encoder.h"

static volatile int value;
K_SEM_DEFINE(press_sem, 0, 1);   /* starts at 0, holds at most 1 press */

static void rotation_cb(struct input_event *evt, void *user_data) {
  value += evt->value;
}

static void button_cb(struct input_event *evt, void *user_data) {
  if (evt->value) {   /* 1 = pressed, 0 = released */
    k_sem_give(&press_sem);
  }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(encoder)), rotation_cb, NULL);
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(buttons)), button_cb, NULL);

int encoder_get(void) { return value; }

bool encoder_button_pressed(void) {
  return k_sem_take(&press_sem, K_NO_WAIT) == 0;
}
