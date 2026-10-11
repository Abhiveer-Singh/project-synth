#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "displaying.h"

static const struct device *display_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint8_t font_w, font_h;
static int rows;

int display_init(void) {
  if (!device_is_ready(display_dev)) {
    printk("Display not ready\n");
    return -1;
  }

  display_blanking_off(display_dev);

  if (cfb_framebuffer_init(display_dev)) {
    printk("Framebuffer init failed\n");
    return -1;
  }

  cfb_framebuffer_set_font(display_dev, 0);
  cfb_get_font_size(display_dev, 0, &font_w, &font_h);
  rows = cfb_get_display_parameter(display_dev, CFB_DISPLAY_HEIGH) / font_h;

  return 0;
}

int display_rows(void) { return rows; }

void display_clear(void) { cfb_framebuffer_clear(display_dev, false); }

void display_line(int row, const char *text) {
  cfb_print(display_dev, text, 0, row * font_h);
}

void display_flush(void) { cfb_framebuffer_finalize(display_dev); }
