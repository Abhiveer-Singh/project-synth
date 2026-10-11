#include <stdio.h>

#include "displaying.h"
#include "menu.h"

#define MENU_ITEMS 5

static const char *items[MENU_ITEMS] = {
    "Sample1", "Sample2", "Sample3", "Sample4", "Sample5",
};

static int selected = -1;
static int top;

static int to_index(int count) {
  int i = count % MENU_ITEMS;
  return i < 0 ? i + MENU_ITEMS : i;
}

static void draw(void) {
  char buf[20];
  int visible = display_rows();

  if (visible > MENU_ITEMS) {
    visible = MENU_ITEMS;
  }

  /* scroll the window so the selected item is always visible */
  if (selected < top) {
    top = selected;
  } else if (selected >= top + visible) {
    top = selected - visible + 1;
  }

  display_clear();

  for (int row = 0; row < visible; row++) {
    int idx = top + row;
    snprintf(buf, sizeof(buf), "%c %s", idx == selected ? '>' : ' ',
             items[idx]);
    display_line(row, buf);
  }

  display_flush();
}

void menu_init(void) {
  selected = 0;
  top = 0;
  draw();
}

void menu_update(int count) {
  int idx = to_index(count);

  if (idx != selected) {
    selected = idx;
    draw();
  }
}
