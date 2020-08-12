all:
	gcc -Wall -g -o mouseemu mouseemu.c
clean:
	rm -f *.o core* mouseemu
install:
	cp -f mouseemu $(DESTDIR)/usr/sbin/
	cp -f mouseemu.8 $(DESTDIR)/usr/share/man/man8
