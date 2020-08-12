/* ----------------------------------------------------------------------------
 * emumouse.c
 * emulates middle click and right click
 *
 * Copyright 2003, 2004 Colin Leroy (colin@colino.net).
 * scan_for_devs() Copyright 2002 Matthias Grimm (joker@cymes.de).
 * Mouse grabbing from Nicholas Hemsley <nick@blackisha.com>
 * Keyboard grabbing by Danny Tholen <obiwan@mailmij.org>
 * 
 * Many thanks to uinput author, <aris@cathedrallabs.org>, for his
 * really useful help.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 * ----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <wait.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "mouseemu.h"
#include "defkeys.h"

static int use_defaults		= 0;

static int b2_mod 		= BUTTON2MOD;
static int b2_key 		= BUTTON2KEY;

static int b3_mod 		= BUTTON3MOD;
static int b3_key 		= BUTTON3KEY;
static int scroll_mod 		= SCROLLMOD;

static int b2_mod_pressed	= 0;
static int b3_mod_pressed	= 0;
static int scroll_mod_pressed	= 0;
static int typing_block_delay   = 300;

char * uinputdev		= NULL;
static int ui_mouse_fd 		= -1;
static int ui_keyb_fd		= -1;

static int running 		= -1;
volatile sig_atomic_t		answer = 1;
volatile sig_atomic_t		rescan = 0;
pid_t pid		= -1;
#define EVENT_DEVS 32
static kdev eventdevs[EVENT_DEVS];
static input_handler ihandler[EVENT_DEVS];

static int debug	= 0;
static int autorescan   = 0;

/* print debug messages to syslog or stderr */
void debugf(const char *format, ...) {
	va_list ap;
	
	if (debug) {
		va_start(ap, format);	
		vsyslog(LOG_DEBUG, format, ap);
		va_end(ap);
	}
}

static void send_event(int fd, int type, int code, int value)
{
	struct input_event event;

	if (fd < 0)
		return;

	event.type = type;
	event.code = code;
	event.value = value;
	gettimeofday(&event.time, NULL);
	if (write(fd, &event, sizeof(event)) < sizeof(event))
		perror("send_event");
		
}

static void passthrough(int fd, struct input_event event)
{
	if (write(fd, &event, sizeof(event)) < sizeof(event))
		perror("passthrough error");

}

static void report_click(int btn, int down)
{
	send_event(ui_mouse_fd, EV_KEY, btn, down);
	send_event(ui_mouse_fd, EV_SYN, SYN_REPORT, 0);
}

static unsigned long long last_scroll = 0;

static void report_scroll (int dy)
{
	struct timeval now;
	static int lastdy = 0, prevdy = 0;
	int cur = 0;
	
	cur = (lastdy + prevdy + dy);
	if (cur > 0)
		cur = 1;
	else if (cur < 0)
		cur = -1;
	
	gettimeofday(&now, NULL);
	
	if ((now.tv_sec*1000000 + now.tv_usec) - last_scroll > 75000) {
		last_scroll = (now.tv_sec*1000000 + now.tv_usec);
		send_event(ui_mouse_fd, EV_REL, REL_WHEEL, -cur);
		send_event(ui_mouse_fd, EV_SYN, SYN_REPORT, 0);
	}
	
	prevdy = lastdy;
	lastdy = dy;
}

int event_parse(int code, int pressed)
{
	int retval = 0;
	
	if (!code) 
		return 1;
	
	if (code == b2_mod && b2_mod) {
		b2_mod_pressed = pressed;
		retval = 1;
	}
	if (code == b3_mod && b3_mod) {
		b3_mod_pressed = pressed;
		retval = 1;
	}
	if (code == scroll_mod && scroll_mod) {
		scroll_mod_pressed = pressed;
		retval = 1;
	}
	
	if (code == b2_key && (b2_mod_pressed || !b2_mod)) {
		report_click(BTN_MIDDLE, pressed);
		retval = 1;
	}
	if (code == b3_key && (b3_mod_pressed || !b3_mod)) {
		report_click(BTN_RIGHT, pressed);
		retval = 1;
	}

	return retval;
}

static unsigned long long last_key = 0;

static int is_modifier(struct input_event event)
{
	switch (event.code) {
		case KEY_LEFTCTRL:
		case KEY_LEFTSHIFT:
		case KEY_LEFTALT:
		case KEY_LEFTMETA:
		case KEY_RIGHTCTRL:
		case KEY_RIGHTSHIFT:
		case KEY_RIGHTALT:
		case KEY_RIGHTMETA:
		case KEY_COMPOSE:
		case KEY_CAPSLOCK:
		case KEY_NUMLOCK:
		case KEY_SCROLLLOCK:
			return 1;
		default:
			return 0;
	}
}

void keyboard_handler (int fd)
{
	struct input_event inp;
	if (read(fd, &inp, sizeof(inp)) == sizeof(inp)) {
		if (!event_parse(inp.code, inp.value) && !is_modifier(inp)) {
			last_key = (inp.time.tv_sec*1000000 + inp.time.tv_usec);
		}
	/* I think its best not to pass scroll, or experiment with not passing the release if
	 * we actually used it for scrolling (but some apps may get stuck?)
	 */
		if (inp.code != b2_key && inp.code != b3_key) {
			passthrough(ui_keyb_fd, inp);
		}
	}
}

static void mouse_handler (int fd)
{
	int count;
	struct input_event inp;

	if ((count = read(fd, &inp, sizeof(inp))) == sizeof(inp)) {
		if (inp.type == EV_KEY && inp.code == BTN_LEFT) {
			if (b2_key == BTN_LEFT && b2_mod_pressed)
				report_click(BTN_MIDDLE, inp.value);
			else if (b3_key == BTN_LEFT && b3_mod_pressed)
				report_click(BTN_RIGHT, inp.value);
			else
				passthrough(ui_mouse_fd, inp);
		}
		else if (scroll_mod_pressed 
		      && inp.type == EV_REL 
		      && (inp.code == REL_Y || inp.code == REL_X)) {
			report_scroll (inp.value);
			//printf("inp.value %d\n", inp.value);
		} else {
			if ((inp.time.tv_sec*1000000+inp.time.tv_usec)-last_key > typing_block_delay*1000 
			|| inp.type == EV_REL)
				passthrough(ui_mouse_fd, inp);
		}
	}
}

void scan_for_devs()
{
	int n, m, fd;
	char filename[20];
	unsigned long bit[NBITS(EV_MAX)];
	unsigned short id[EVENT_DEVS];

	for (n = 0, m = 0; n < EVENT_DEVS; n++) {
		sprintf(filename, "/dev/input/event%d", n);
		if ((fd = open(filename, O_RDONLY)) >= 0) {
			ioctl(fd, EVIOCGBIT(0, EV_MAX), bit);
			if (test_bit(EV_KEY, bit) && test_bit(EV_REP, bit)) {
				ioctl(fd, EVIOCGID, id);
				/* our own virtual keyboard (on rescans)*/
				if (id[ID_PRODUCT] == 0x1F && id[ID_VENDOR] == 0x1F) {
					close(fd);
					continue;
				}
				if (id[ID_PRODUCT] != eventdevs[m].product ||
					id[ID_VENDOR]  != eventdevs[m].vendor) {
					if (eventdevs[m].handle >= 0) {
						unregister_inputhandler(eventdevs[m].handle);
						close(eventdevs[m].handle);
					}
					debugf("keyboard: fd %d event%d, vendor %4x product %4x\n", fd, n, id[ID_VENDOR], id[ID_PRODUCT]);
					eventdevs[m].handle= fd;
					eventdevs[m].product = id[ID_PRODUCT];
					eventdevs[m].vendor = id[ID_VENDOR];
					register_inputhandler(fd, keyboard_handler, 1);
				}
				m++;
			} else if (test_bit(EV_REL, bit)) {
				ioctl(fd, EVIOCGID, id);
				/* our own virtual mouse (on rescans)*/
				if (id[ID_PRODUCT] == 0x1E && id[ID_VENDOR] == 0x1F) {
					close(fd);
					continue;
				}
				if (id[ID_PRODUCT] != eventdevs[m].product ||
					id[ID_VENDOR]  != eventdevs[m].vendor) {
					if (eventdevs[m].handle >= 0) {
						unregister_inputhandler(eventdevs[m].handle);
						close(eventdevs[m].handle);
					}
					debugf("mouse   : fd %d event%d, vendor %4x product %4x\n", fd, n, id[ID_VENDOR], id[ID_PRODUCT]);
					eventdevs[m].handle= fd;
					eventdevs[m].product = id[ID_PRODUCT];
					eventdevs[m].vendor = id[ID_VENDOR];
					register_inputhandler(fd, mouse_handler, 1);
				}
				m++;		
			} else
				close(fd);
		}
	}
	for (; m < EVENT_DEVS; m++) {    /* cleanup rest of list */
		eventdevs[m].product = 0;
		eventdevs[m].vendor  = 0;
		eventdevs[m].handle  = -1;
	}
}

void rescan_devs()
{
	int i, cfd;

        for (i=0; i<EVENT_DEVS; i++) {
		if (ihandler[i].fd != -1) {
			cfd=ihandler[i].fd;
			unregister_inputhandler(ihandler[i].fd);
			close(cfd);
			i--;
		}
	}
        for (i=0; i<EVENT_DEVS; i++) {
		eventdevs[i].product = 0;
		eventdevs[i].vendor  = 0;
		eventdevs[i].handle  = -1;
	}
	usleep(100);
	scan_for_devs();
}

int register_inputhandler (int fd, void (*func)(int fd), int grab)
{
	int n;

	for (n=0; n < EVENT_DEVS; n++)
		if (ihandler[n].fd == -1) {
			ihandler[n].fd = fd;
      			ihandler[n].handler = func;
			ihandler[n].grab = grab;
			if (grab)
				ioctl(fd, EVIOCGRAB, 1);
	      		return 0;
    		}
	return -1;
}

void unregister_inputhandler (int fd)
{
	int n, found = 0;

	for (n = 0; n < EVENT_DEVS; n++)
		if (found) {
			ihandler[n-1].fd = ihandler[n].fd;
			ihandler[n-1].handler = ihandler[n].handler;
		} else if (ihandler[n].fd == fd) {
			found = 1;
			if (ihandler[n].grab)
				ioctl(fd, EVIOCGRAB, 0);
		}
	if (found)
		ihandler[n].fd = -1;
}

int create_fdset (fd_set *watchset)
{
	int n, maxfd;

	FD_ZERO(watchset);
	for (maxfd=n=0; n < EVENT_DEVS; n++) {
		if (ihandler[n].fd == -1) continue;
		FD_SET(ihandler[n].fd, watchset);
		if (ihandler[n].fd > maxfd)
			maxfd = ihandler[n].fd;
	}
	return maxfd;
}

void call_inputhandler(fd_set *inset)
{
	int n;

	for (n=0; n < EVENT_DEVS; n++) {
		if (ihandler[n].fd == -1) continue;
		if (FD_ISSET(ihandler[n].fd, inset))
			ihandler[n].handler (ihandler[n].fd);
	}
}

void uinput_close(int fd)
{
	if (fd) {
		ioctl(fd, UI_DEV_DESTROY, NULL);
		close(fd);
		fd = -1;
	}
}

/*
 * opens uinput device
 * defaults to the specified or default path,
 * then tries some distro-specific paths
 */
int uinput_open_device(void)
{
	int fd = -1;

	syslog(LOG_NOTICE, "Trying to open %s...", uinputdev);
	fd = open (uinputdev, O_RDWR);
	syslog(LOG_NOTICE, " %s.\n", (fd > 0)?"ok":"error");
	if (fd > 0)
		return fd;

	syslog(LOG_NOTICE, "Trying to open /dev/uinput...");
	fd = open("/dev/uinput", O_RDWR);
	syslog(LOG_NOTICE, " %s.\n", (fd > 0)?"ok":"error");
	if (fd > 0)
		return fd;

	syslog(LOG_NOTICE, "Trying to open /dev/input/uinput...");
	fd = open("/dev/input/uinput", O_RDWR);
	syslog(LOG_NOTICE, " %s.\n", (fd > 0)?"ok":"error");
	if (fd > 0)
		return fd;

	syslog(LOG_NOTICE, "Trying to open /dev/misc/uinput...");
	fd = open("/dev/misc/uinput", O_RDWR);
	syslog(LOG_NOTICE, " %s.\n", (fd > 0)?"ok":"error");
	if (fd > 0)
		return fd;

	return -1;
}

int uinput_setup(void)
{
	struct uinput_user_dev device;
	int i;
	
	/*setup keyboard device */
	
        if(ui_keyb_fd > 0) {
		uinput_close(ui_keyb_fd);
	}

	ui_keyb_fd = uinput_open_device();
	
	if (ui_keyb_fd < 0) {
		perror("open");
		return -1;
	}
	
	strcpy(device.name, "Mouseemu virtual keyboard");

	device.id.bustype = 0;
	device.id.vendor = 0x1F;
	device.id.product = 0x1F;
	device.id.version = 0;
	
	if (write(ui_keyb_fd, &device, sizeof(device)) < sizeof(device)) {
		perror("write");
		close(ui_keyb_fd);
		return -1;
	}

	/* We need to create a virtual keyboard. Set up key, repeat
	 * and placeholders for all keys
	 */
	
        ioctl(ui_keyb_fd, UI_SET_EVBIT, EV_KEY);
        ioctl(ui_keyb_fd, UI_SET_EVBIT, EV_REP);
        for (i = KEY_RESERVED; i <= KEY_UNKNOWN; i++)
		if (ioctl(ui_keyb_fd, UI_SET_KEYBIT, i) != 0) {
			perror("ioctl UI_SET_KEYBIT");
			close(ui_keyb_fd);
			return -1;
		}
        
	ioctl(ui_keyb_fd, UI_DEV_CREATE, NULL);
	
	/*setup mouse device*/


	if(ui_mouse_fd > 0) {
		uinput_close(ui_mouse_fd);
	}
		
	ui_mouse_fd = uinput_open_device();

        if (ui_mouse_fd < 0) {
		perror("open");
		return -1;
	}

        strcpy(device.name, "Mouseemu virtual mouse");

        device.id.bustype = 0;
        device.id.vendor = 0x1F;
        device.id.product = 0x1E;
        device.id.version = 0;

        if (write(ui_mouse_fd, &device, sizeof(device)) < sizeof(device)) {
		perror("write");
		close(ui_mouse_fd);
		return -1;
	}
	
	
	/* mousedev only recognize our device as a mouse
	 * if it has at least two axis and one left button.
	 * Also, mouse buttons have to send sync events. 
	 */
	ioctl(ui_mouse_fd, UI_SET_EVBIT, EV_KEY);
	ioctl(ui_mouse_fd, UI_SET_EVBIT, EV_REL);
	ioctl(ui_mouse_fd, UI_SET_EVBIT, EV_SYN);

	ioctl(ui_mouse_fd, UI_SET_RELBIT, REL_X);
	ioctl(ui_mouse_fd, UI_SET_RELBIT, REL_Y);
	ioctl(ui_mouse_fd, UI_SET_RELBIT, REL_WHEEL);
	
	ioctl(ui_mouse_fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(ui_mouse_fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(ui_mouse_fd, UI_SET_KEYBIT, BTN_MIDDLE);

	ioctl(ui_mouse_fd, UI_DEV_CREATE, NULL);

	return 1;
}

void uinput_cleanup()
{
	int i, cfd;
	
	syslog(LOG_NOTICE, "mouseemu: cleaning...\n");

        uinput_close(ui_keyb_fd);
        uinput_close(ui_mouse_fd);

        for (i=0; i<EVENT_DEVS; i++) {
		if (ihandler[i].fd != -1) {
			cfd=ihandler[i].fd;
			unregister_inputhandler(ihandler[i].fd);
			close(cfd);
		}
	}

        exit(0);
}
	
/* signal handler to ensure that mouse and keyboard keep responding
 * answer==1 keeps child and parent running, 0 aborts
 * 
 * note that monitor receives signals in both the parent 
 * and the child process
 * 
 */
void monitor(int sig_num)
{
	if (sig_num == SIGUSR1) {
		answer = 1;
	} else if (sig_num == SIGHUP) {
		rescan = 1;
	} else if (sig_num == SIGALRM) {
		rescan = 1;
		alarm(5);
	} else {
		//printf("mouseemu: aborting on sig %i \n",sig_num);	
		/*terminate the parent:*/
		answer = 0;
		/*and the child*/
		if (pid == 0)
			uinput_cleanup();
	}
}
			
void install_sighandler(void)
{
	struct sigaction usr_action;
	sigset_t mask;

	sigfillset(&mask);

	usr_action.sa_handler = monitor;
	usr_action.sa_mask = mask;
	usr_action.sa_flags = 0;

	/*SIGUSR1 for process communication
	 *SIGTERM and SIGCHLD for quitting
	 *SIGHUP and SIGALRM for rescaning devices
	 */
	sigaction(SIGUSR1, &usr_action, NULL);
	sigaction(SIGTERM, &usr_action, NULL);
	sigaction(SIGHUP,  &usr_action, NULL);
	sigaction(SIGCHLD, &usr_action, NULL);
	sigaction(SIGALRM, &usr_action, NULL);
	
	sigprocmask(SIG_UNBLOCK, &mask, 0);	

}

/* print usage message to stdout/stderr */
void usage(FILE *stream, char *argv[]) {
	fprintf(stream, "usage: %s \n"
	                "\t[-middle B2_MOD B2_KEY]\n"
	                "\t[-right	B3_MOD B3_KEY]\n"
	                "\t[-scroll SCROLL_MOD]\n"
	                "\t[-typing-block DELAY]\n"
	                "\t[-device UINPUT_DEVICE]\n"
	                "\t[-nofork]\n"
			"\t[-autorescan]\n"
			"\t[-debug]\n",
					argv[0]);
	fprintf(stream, "All modifier and button key arguments are\n"
	                "key scancodes. They can be found in \n"
	                "/usr/src/linux/include/linux/input.h,\n"
	                "or by using `showkey` in a console.\n"
	                "Use decimal values. BTN_LEFT(272) is usable as "
	                "B2_KEY or B3_KEY.\n\n");
	fprintf(stream, "Default uinput device: " DEFAULT_UINPUT ".\n");
	fprintf(stream, "Default keys:\n");
	if (use_defaults)
		fprintf(stream,
	                "\tMiddle click : F10 (0 68)\n"
	                "\tRight click	: F11 (0 87)\n");
	else
		fprintf(stream,
	                "\tMiddle click : none (0 0)\n"
	                "\tRight click	: none (0 0)\n");
	fprintf(stream, "\tScroll mod.	: Alt (56)\n"
	                "\tDefault blocking time while typing: 300ms\n");

	exit(0);
}

int main(int argc, char *argv[])
{
	int i=0, maxfd, val;
	fd_set inset;
	struct timeval tv;
	pid_t fpid;
	int nofork = 0;
	//int argv0size = strlen(argv[0]);

	install_sighandler();

#ifdef __powerpc__
	use_defaults = 1;
#else
#if defined(__i386__) || defined(__amd64__)
	{
		FILE *dmidecode;
		char line[1024];
		dmidecode = popen("dmidecode -s system-manufacturer 2>/dev/null", "r");
		if (dmidecode) {
			if (fgets(line, 1024, dmidecode) && !strncmp(line, "Apple", 5))
				use_defaults = 1;
			pclose(dmidecode);
		}
	}
#endif
#endif
	if (!use_defaults) {
		b2_mod = 0;
		b2_key = 0;
		b3_mod = 0;
		b3_key = 0;
	}

	uinputdev = DEFAULT_UINPUT;
	if (argc > 1) {
		int i = 0;
		if (!strcmp(argv[1],"-help")) {
			usage(stdout, argv);	
		} else {
			i = 1;
			while (i < argc) {
				if (!strcmp(argv[i], "-middle")) {
					if (argc > i+2) {
						b2_mod = atoi(argv[i+1]);
						b2_key = atoi(argv[i+2]);
						i += 3;
					} else 
						usage(stderr, argv);
					continue;
				} 
				else if (!strcmp(argv[i], "-right")) {
					if (argc > i+2) {
						b3_mod = atoi(argv[i+1]);
						b3_key = atoi(argv[i+2]);
						i += 3;
					} else 
						usage(stderr, argv);
					continue;
				}
				else if (!strcmp(argv[i], "-scroll")) {
					if (argc > i+1) {
						scroll_mod = atoi(argv[i+1]);
						i += 2;
					} else 
						usage(stderr, argv);
					continue;					
				}
				else if (!strcmp(argv[i], "-typing-block")) {
					if (argc > i+1) {
						typing_block_delay = atoi(argv[i+1]);
						i += 2;
					} else 
						usage(stderr, argv);
					continue;					
				}
				else if (!strcmp(argv[i], "-device")) {
					if (argc > i+1) {
						uinputdev = argv[i+1];
						i += 2;
					} else 
						usage(stderr, argv);
					continue;					
				}
				else if (!strcmp(argv[i], "-nofork")) {
					nofork=1;
					i += 1;
					continue;
				}
				else if (!strcmp(argv[i], "-autorescan")) {
					autorescan=1;
					i += 1;
					continue;
				}
				else if (!strcmp(argv[i], "-debug")) {
					debug=1;
					i += 1;
					continue;
				} else {
					usage(stderr, argv);
                }
			}
		}
	}
	
	if (nofork) 
		openlog("mouseemu", LOG_NDELAY | LOG_PERROR | LOG_PID, LOG_DAEMON); 
	else
		openlog("mouseemu", LOG_NDELAY | LOG_PID, LOG_DAEMON);
	
	syslog(LOG_NOTICE, "mouseemu " VERSION " (C) Colin Leroy <colin@colino.net>\n");
	syslog(LOG_NOTICE, "using (%d+%d) as middle button, (%d+%d) as right button, (%d) as scroll.\n",
		b2_mod, b2_key, b3_mod, b3_key, scroll_mod);

	if (nofork)
		goto startops;

	fpid = fork();
	if (fpid == -1) {
		syslog(LOG_NOTICE, "can't fork\n");
		goto startops;
	}
	if (fpid != 0) {
		exit(0);
	}

	setsid();
	pid = fork();
	if (pid == -1) {
		syslog(LOG_NOTICE, "can't fork\n");
		goto startops;
	}

	if (pid != 0) {
		/* parent. Since the death of the child process can leave our keyboard 
		 * and mouse unusable, a signal is send. if the return signal
		 * doesn't set answer to 0 within 10 secs, we exit
		 * 
		 */
			
		struct sigaction sa;
		sigset_t mask, oldmask;
		FILE *pidfile;
			       
		/* SIGHUP and SIGALRM are only useful in the child */
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_IGN;
		sigaction(SIGHUP, &sa, NULL);
		sigaction(SIGALRM, &sa, NULL);

		/* write PID file so the user can signal us for device rescans */
		pidfile = fopen("/var/run/mouseemu.pid", "w");
		if (!pidfile) {
			perror("mouseemu: can't open /var/run/mouseemu.pid");
			exit(1);
		}
		fprintf(pidfile, "%d\n", pid);
		fclose(pidfile);

		/*we start only after we received the first sigusr1 from child:*/

		sigemptyset(&mask);
		sigaddset(&mask, SIGUSR1);
		sigprocmask(SIG_BLOCK, &mask, &oldmask);
		sigsuspend(&oldmask);
		sigprocmask(SIG_UNBLOCK, &mask, NULL);

		while (answer) {
			int must_sleep = 10;
			
			answer=0;
			
			if (kill(pid, SIGUSR1)<0)
				break;

			while (must_sleep > 0) {
				must_sleep = sleep(must_sleep);
			}
			
		}
		syslog(LOG_NOTICE, "terminating, %i\n",answer);	
		if (kill(pid, SIGTERM)<0)
			perror("mouseemu: termination of uinput handlers failed\n");
			
		waitpid(pid, NULL, 0);
		exit(0);
	}

	/*change the child process name*/
	
	//strncpy(argv[0],"mouseemu",argv0size);
startops:

	for (i=0; i<EVENT_DEVS; i++) {
		eventdevs[i].handle = -1;
		eventdevs[i].vendor = 0;
		eventdevs[i].product= 0;
					                
		ihandler[i].handler=0;
		ihandler[i].fd=-1;
	}

	sleep(1);

	scan_for_devs();

	running = uinput_setup();
	if (running < 0) {
		syslog(LOG_NOTICE, "Make sure uinput module is loaded or available "
		                   "in the kernel.\n");
	}
                                                         

	chdir("/");
	
	if (autorescan) 
		alarm(5);
	
	/*main loop*/
	
        while(running > 0) {
	
		tv.tv_sec = 1; tv.tv_usec = 0;
		maxfd = create_fdset(&inset);
		val = select (maxfd+1, &inset, NULL, NULL, &tv);
		/* signal received, so rescan for devices when idle*/
		if (val == 0 && rescan) {
			rescan = 0;
			rescan_devs();
		}
		if (val >= 0) {
			if (val == 0)
				usleep(10);
			else {
				if (errno == ENODEV) {
					debugf("device disconnect detected (select %d, errno %d), rescanning devices\n", val, errno);
					errno = 0;
					rescan_devs();
					usleep(500);
				} else {
					call_inputhandler(&inset);
				}
			}
		}
		/* tell the parent we are running without problems */
		/* What should we do if the parent is dead? */
		if (answer && !nofork) {
			answer=0;
			kill(getppid(), SIGUSR1);
		}

	}

	uinput_cleanup();

	exit(0);
}

