#ifndef __DRIVERS__SERIAL_H__
#define __DRIVERS__SERIAL_H__

#include <stdint.h>

void serial_out(uint8_t b);

#if bios == 1
int serial_in(void);
#endif

#endif
