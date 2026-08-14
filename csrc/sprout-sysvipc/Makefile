# Rebuild the sprout-sysvipc shim DSOs for both guest ABIs.
#
# Note: NOT built by install.sh (guest-side artifact, runs under the guest's
# own box64, needs the cross toolchain inside the guest). Run manually when
# the sysvipc shim changes:
#
#   make -C csrc/sprout-sysvipc install ROOTFS=/path/to/rootfs
#
# Requires x86_64-linux-gnu-gcc + i686-linux-gnu-gcc in the path. Inside the
# sprout-run debian guest: `apt install gcc-x86-64-linux-gnu gcc-i686-linux-gnu`.

CC64 ?= x86_64-linux-gnu-gcc
CC32 ?= i686-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -fPIC
LDFLAGS ?= -shared

DESTDIR ?= /usr/lib/sprout-sysvipc
ROOTFS  ?=

.PHONY: all install clean check

all: build/x86_64/libsprout-sysvipc.so build/i386/libsprout-sysvipc.so

build/x86_64/libsprout-sysvipc.so: sprout_sysvipc.c
	@mkdir -p build/x86_64
	$(CC64) $(CFLAGS) $(LDFLAGS) -o $@ $<

build/i386/libsprout-sysvipc.so: sprout_sysvipc.c
	@mkdir -p build/i386
	$(CC32) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: all
	@for a in x86_64 i386; do \
		dst="$(ROOTFS)$(DESTDIR)/$$a"; \
		mkdir -p "$$dst"; \
		install -m 0755 "build/$$a/libsprout-sysvipc.so" "$$dst/libsprout-sysvipc.so"; \
		echo "install: $$dst/libsprout-sysvipc.so"; \
	done

check: all
	@command -v x86_64-linux-gnu-gcc >/dev/null || { echo "missing x86_64 cross gcc"; exit 1; }
	@command -v i686-linux-gnu-gcc   >/dev/null || { echo "missing i686 cross gcc";   exit 1; }
	@for t in build/x86_64/semtest build/i386/semtest; do (make $$t) ; done

build/x86_64/semtest: semtest.c
	@mkdir -p build/x86_64
	$(CC64) -O2 -Wall -Wextra -o $@ $<
build/i386/semtest: semtest.c
	@mkdir -p build/i386
	$(CC32) -O2 -Wall -Wextra -o $@ $<

clean:
	rm -rf build
