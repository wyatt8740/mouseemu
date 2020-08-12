/* ----------------------------------------------------------------------------
 * emumouse.h
 * emulates middle click and right click
 *
 * Copyright 2003, 2004 Colin Leroy (colin@colino.net).
 *
 * Many thanks to uinput author, <aris@cathedrallabs.org>, for his
 * really useful help.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 * ----------------------------------------------------------------------------*/
#ifndef __MOUSEEMU_H__
#define __MOUSEEMU_H__

#undef DEBUG
#define VERSION "0.15"

#define BITS_PER_LONG (sizeof(long) * 8)

#ifndef NBITS
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#endif

#define OFF(x) ((x)%BITS_PER_LONG)
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

#define DEFAULT_UINPUT "/dev/uinput"
#define BTN1 0x01
#define BTN2 0x04
#define BTN3 0x02

/* device structure */
typedef struct _kdev {
	int handle;
	unsigned short vendor;
	unsigned short product;
} kdev;

/* handler structure */
typedef struct _ihandler {
	void (*handler)(int fd);
	int fd;
	int grab;
} input_handler;

void unregister_inputhandler (int fd);
int register_inputhandler(int fd, void (*func)(int fd), int grab);
#endif 
