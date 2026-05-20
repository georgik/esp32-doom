#ifndef TSINPUT_H
#define TSINPUT_H

// Bitmask constants for touch button detection (active low - pressed = bit cleared)
#define BUT_UP        (1<<0)
#define BUT_DOWN      (1<<1)
#define BUT_LEFT      (1<<2)
#define BUT_RIGHT     (1<<3)
#define BUT_START     (1<<4)
#define BUT_SELECT    (1<<5)
#define BUT_TRIANGLE  (1<<6)
#define BUT_CROSS     (1<<7)
#define BUT_CIRCLE    (1<<8)
#define BUT_SQUARE    (1<<9)
#define BUT_L1        (1<<10)
#define BUT_R1        (1<<11)

void tsJsInputInit();
int tsJsInputGet();

#endif
