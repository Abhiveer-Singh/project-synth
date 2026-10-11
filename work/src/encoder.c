#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "encoder.h"

static atomic_t value;

static void encoder_cb(struct input_event *evt, void *user_data) {
  if (evt->type == INPUT_EV_REL && evt->code == INPUT_REL_X) {
    /* clockwise gives a positive value, anticlockwise a negative one */
    atomic_add(&value, evt->value);
    printk("Encoder: %d\n", (int)atomic_get(&value));
  }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(encoder)), encoder_cb, NULL);

int encoder_get(void) { return (int)atomic_get(&value); }
