CC ?= cc
PKG_CONFIG ?= pkg-config
GTK4_MIN_VERSION ?= 4.24.1
STATIC ?= 0
TARGET := rawreader
SOURCES := src/main.c src/app.c src/block_device.c src/device.c src/fat32.c src/raw_reader.c src/hex_dump.c
OBJECTS := $(SOURCES:.c=.o)
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4 2>/dev/null)

ifeq ($(STATIC),1)
TARGET := rawreader-static
GTK_LIBS := $(shell $(PKG_CONFIG) --static --libs gtk4 2>/dev/null)
ifeq ($(shell uname -s),Darwin)
GTK_LIBS := $(shell sh tools/static-macos-libs.sh $(PKG_CONFIG) gtk4 2>/dev/null)
else
LDFLAGS += -static
endif
else
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk4 2>/dev/null)
endif

CPPFLAGS += -D_FILE_OFFSET_BITS=64 -Iinclude $(GTK_CFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(GTK_LIBS)

.PHONY: all clean check check-deps check-static-libs static-linux static-macos

all: $(TARGET)

check: check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -o test-storage tests/test_storage.c src/block_device.c src/fat32.c $(LDLIBS)
	./test-storage
	rm -f test-storage

check-deps:
	@$(PKG_CONFIG) --atleast-version=$(GTK4_MIN_VERSION) gtk4 || { \
		echo "GTK $(GTK4_MIN_VERSION) or newer development files are required (pkg-config gtk4)." >&2; \
		exit 1; \
	}

check-static-libs: check-deps
ifeq ($(STATIC),1)
ifeq ($(shell uname -s),Darwin)
	@sh tools/static-macos-libs.sh $(PKG_CONFIG) gtk4 >/dev/null
else
	@echo "Linux static linking requires .a archives for GTK 4 and every linked dependency."
endif
endif

ifeq ($(STATIC),1)
$(OBJECTS): | check-static-libs
else
$(OBJECTS): | check-deps
endif

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

static-linux:
	@test "$$(uname -s)" = "Linux" || { echo "Run static-linux on Linux." >&2; exit 1; }
	$(MAKE) clean
	$(MAKE) STATIC=1 all

static-macos:
	@test "$$(uname -s)" = "Darwin" || { echo "Run static-macos on macOS." >&2; exit 1; }
	$(MAKE) clean
	$(MAKE) STATIC=1 all

clean:
	rm -f rawreader rawreader-static test-storage $(OBJECTS)
