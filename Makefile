CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
PORTABILITY_CPPFLAGS := -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INCLUDEDIR ?= $(PREFIX)/include
LIBEXECDIR ?= $(PREFIX)/libexec/c-buildsystem
BUILD := build
TARGET := $(BUILD)/c
NATIVE := $(BUILD)/c-native
UNAME_S := $(shell uname -s)
LDLIBS :=
ifeq ($(UNAME_S),Linux)
LDLIBS += -ldl
endif

.PHONY: all clean install uninstall test

all: $(TARGET)

$(NATIVE): src/cli.c src/main.c src/cache_io.h src/perf_v2.h include/cbuild.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PORTABILITY_CPPFLAGS) $(CFLAGS) -include src/cache_io.h -Iinclude -DCBUILD_HEADER_PATH='"$(abspath include/cbuild.h)"' src/cli.c $(LDLIBS) -o $(NATIVE)

$(TARGET): src/wrapper.c src/wrapper_compat.h $(NATIVE)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PORTABILITY_CPPFLAGS) $(CFLAGS) -include src/wrapper_compat.h src/wrapper.c -o $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCLUDEDIR) $(DESTDIR)$(LIBEXECDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/c
	install -m 755 $(NATIVE) $(DESTDIR)$(LIBEXECDIR)/c-native
	rm -f $(DESTDIR)$(INCLUDEDIR)/cbuild.h
	install -m 644 include/cbuild.h $(DESTDIR)$(INCLUDEDIR)/cbuild.h

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/c $(DESTDIR)$(INCLUDEDIR)/cbuild.h $(DESTDIR)$(LIBEXECDIR)/c-native
	-rmdir $(DESTDIR)$(LIBEXECDIR) 2>/dev/null

clean:
	rm -rf $(BUILD)

test: $(TARGET)
	sh .github/ci/run-tests.sh $(abspath $(TARGET)) $(abspath include)
