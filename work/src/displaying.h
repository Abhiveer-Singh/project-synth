#ifndef DISPLAYING_H
#define DISPLAYING_H

int display_init(void);
int display_rows(void);
void display_clear(void);
void display_line(int row, const char *text);
void display_flush(void);

#endif
