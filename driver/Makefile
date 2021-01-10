EXTRA_CFLAGS := -pipe -m64 -ansi -fPIC -O2 -fno-exceptions -fstack-protector -Wl,-z,relro -Wl,-z,now -fvisibility=internal -W -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-label -Wpointer-arith -Wformat -Wreturn-type -Wsign-compare -Wmultichar -Wformat-nonliteral -Winit-self -Wuninitialized -Wno-deprecated -Wformat-security -Werror -Wall -Winline -Wextra -Wshadow -funsigned-char -fwrapv -Wmissing-format-attribute -Wwrite-strings -Wenum-compare -Wlogical-op -Wsuggest-attribute=noreturn -Wunreachable-code -fno-omit-frame-pointer

NAME=dmap
KDIR ?= /lib/modules/`uname -r`/build
KVERSION = $(shell uname -r)

default: target install

target:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

help:
	$(MAKE) -C $(KDIR) M=$$PWD help

install:	
	/lib/modules/`uname -r`/source/scripts/sign-file sha512  /lib/modules/`uname -r`/source/certs/signing_key.pem  /lib/modules/`uname -r`/source/certs/signing_key.x509 dmap.ko
	if [ -f $(NAME).ko ]; then \
		if ! cp $(NAME).ko /lib/modules/$(KVERSION)/kernel/drivers/char; \
			then exit 1; fi; \
	fi;
	/sbin/depmod -a

uninstall:
	if [ -f /lib/modules/$(KVERSION)/kernel/drivers/char/$(NAME).ko ]; then \
		if ! rm -f /lib/modules/$(KVERSION)/kernel/drivers/char/$(NAME).ko; \
			then exit 1; fi; \
	fi;
	/sbin/depmod -a
