#include <zephyr/kernel.h>

#include "displaying.h"
#include "encoder.h"
#include "menu.h"

int main(void) {
  if (display_init()) {
    return 0;
  }

  menu_init();

  while (1) {
    menu_update(encoder_get());
    k_msleep(20);
  }
}
