# Builds the gba2cia player inside the devkitpro/devkitarm image (see build.sh).
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
BUILD := build
MGBA := third_party/mgba

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
DEFINES := -D__3DS__ -DBUILD_STATIC -DCOLOR_16_BIT -DCOLOR_5_6_5 -DFIXED_ROM_BUFFER -DM_CORE_GBA \
	-DUSE_VFS_3DS -D_GNU_SOURCE -DIOAPI_NO_64 -DHAVE_STRDUP -DHAVE_STRNDUP -DHAVE_STRLCPY \
	-DHAVE_LOCALTIME_R -DHAVE_VASPRINTF -DHAVE_LOCALE -DHAVE_NEWLOCALE -DHAVE_FREELOCALE \
	-DHAVE_USELOCALE -DHAVE_SETLOCALE -DHAVE_STRTOF_L
CFLAGS := -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format \
	$(ARCH) -ffunction-sections -mword-relocations $(DEFINES) \
	-I$(DEVKITPRO)/libctru/include -I$(MGBA)/include -I$(BUILD)/mgba/include -I$(MGBA)/src
LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections -L$(DEVKITPRO)/libctru/lib

SRC := $(wildcard source/*.c)
OBJ := $(patsubst source/%.c,$(BUILD)/obj/%.o,$(SRC))

all: $(BUILD)/player.elf

$(BUILD)/obj/%.o: source/%.c $(wildcard source/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/player.elf: $(OBJ) $(BUILD)/mgba/libmgba.a
	$(CC) $(LDFLAGS) $(OBJ) $(BUILD)/mgba/libmgba.a -lcitro2d -lcitro3d -lctru -lm -o $@

clean:
	rm -rf $(BUILD)/obj $(BUILD)/player.elf

.PHONY: all clean
