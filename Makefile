CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
BUILD := build

ifeq ($(OS),Windows_NT)
ifeq ($(origin CC),default)
CC := clang
endif
EXE := .exe
TARGET := $(BUILD)/c$(EXE)
NATIVE := $(BUILD)/c-native$(EXE)
PREFIX ?= $(LOCALAPPDATA)/Programs/C-BuildSystem
BINDIR ?= $(PREFIX)/bin
INCLUDEDIR ?= $(PREFIX)/include
LIBEXECDIR ?= $(PREFIX)/libexec/c-buildsystem
WINDOWS_SOURCE := src/windows.c
WINDOWS_INSTALL := src/windows_install.ps1

.PHONY: all clean install uninstall test

all: $(TARGET) $(NATIVE)

$(TARGET): $(WINDOWS_SOURCE) src/windows_platform.h src/windows_build.h include/cbuild.h
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(BUILD)' | Out-Null"
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude $(WINDOWS_SOURCE) -o $(TARGET)

$(NATIVE): $(WINDOWS_SOURCE) src/windows_platform.h src/windows_build.h include/cbuild.h
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(BUILD)' | Out-Null"
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude $(WINDOWS_SOURCE) -o $(NATIVE)

install: all $(WINDOWS_INSTALL)
	powershell -NoProfile -ExecutionPolicy Bypass -File $(WINDOWS_INSTALL) -Prefix "$(PREFIX)" -Target "$(TARGET)" -Native "$(NATIVE)" -Header "include/cbuild.h"

uninstall: $(WINDOWS_INSTALL)
	powershell -NoProfile -ExecutionPolicy Bypass -File $(WINDOWS_INSTALL) -Prefix "$(PREFIX)" -Uninstall

clean:
	powershell -NoProfile -Command "if (Test-Path '$(BUILD)') { Remove-Item -Recurse -Force '$(BUILD)' }; exit 0"

test: all
	powershell -NoProfile -ExecutionPolicy Bypass -File .github/ci/windows.ps1

else
PORTABILITY_CPPFLAGS := -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INCLUDEDIR ?= $(PREFIX)/include
LIBEXECDIR ?= $(PREFIX)/libexec/c-buildsystem
TARGET := $(BUILD)/c
NATIVE := $(BUILD)/c-native
UNAME_S := $(shell uname -s)
LDLIBS :=
ifeq ($(UNAME_S),Linux)
LDLIBS += -ldl
endif

.PHONY: all clean install uninstall test

all: $(TARGET)

$(NATIVE): src/cli.c src/main.c src/cache_io.h src/cache_cleanup.h src/perf_v2.h src/cbuild_target_dep.h include/cbuild.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PORTABILITY_CPPFLAGS) $(CFLAGS) -include src/cache_io.h -include src/cache_cleanup.h -Iinclude -DCBUILD_HEADER_PATH='"$(abspath include/cbuild.h)"' src/cli.c $(LDLIBS) -o $(NATIVE)

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
endif
