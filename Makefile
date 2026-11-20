DEVKITPRO ?= /opt/devkitpro
LIBOUNCE := $(DEVKITPRO)/libounce
PORTLIBS := $(DEVKITPRO)/portlibs/switch
PORTLIBS_SWITCH2 := $(DEVKITPRO)/portlibs/switch2
TOOLS := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-

include $(LIBOUNCE)/switch2_cpu.mk

NAME := junkbot
TITLE := LEGO Junkbot
AUTHOR := mysterypaint, 1j01
VERSION := 1.0.0
TITLEID := 0000000000069421
ICON := icon.png
ROMFS := romfs
OUT := build
SOURCES := $(wildcard source/*.c)
OBJECTS := $(SOURCES:source/%.c=$(OUT)/%.o)
DEPS := $(OBJECTS:.o=.d)

ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := -std=gnu11 -O2 -g -Wall -Wno-unused-parameter -ffunction-sections -fdata-sections \
	$(ARCH) -D__SWITCH__ -D__SWITCH2__ -D_REENTRANT \
	-Iinclude -I$(PORTLIBS_SWITCH2)/include -isystem $(LIBOUNCE)/include \
	-I$(PORTLIBS)/include/SDL2 -I$(PORTLIBS)/include

LIBPATHS := -L$(PORTLIBS_SWITCH2)/lib -L$(LIBOUNCE)/lib -L$(PORTLIBS)/lib
LIBS := -lSDL2_mixer -lvorbisidec -lmodplug -lmpg123 -lopusfile -logg -lopus \
	-lSDL2_image -ljpeg -lwebp \
	-lSDL2_ttf -lfreetype -lbz2 -lpng16 -lz -lharfbuzz \
	-lSDL2 -lEGL -lglapi -ldrm_nouveau -lstdc++ -lpthread \
	-lnx -lm

STAMP := $(OUT)/.assets

.PHONY: all clean assets icon

all: $(OUT)/$(NAME).nro

$(STAMP): Makefile tools/prepare_assets.py tools/dir_extract.py tools/music_timing.py \
		tools/swf_frames.py tools/ruffle_intro.py tools/audio_gain.py tools/make_icon.py \
		tools/data/sprite_recipes.json tools/data/sprite_frames.json include/jb_music_timing.h \
		include/jb_audio_gain.h assets/04B_08__.TTF assets/intro/atlas.png \
		assets/intro/atlas.tsv assets/intro/seq.bin assets/intro/ruffle.txt
	python3 tools/prepare_assets.py
	python3 tools/make_icon.py
	@mkdir -p $(OUT)
	@touch $@

$(OUT)/%.o: source/%.c
	@mkdir -p $(dir $@)
	$(TOOLS)gcc $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/$(NAME).nacp: Makefile
	@mkdir -p $(OUT)
	$(DEVKITPRO)/tools/bin/nacptool --create "$(TITLE)" "$(AUTHOR)" "$(VERSION)" $@ --titleid=$(TITLEID)

$(OUT)/$(NAME).elf: $(OBJECTS)
	$(TOOLS)gcc -specs=$(LIBOUNCE)/switch.specs -g $(ARCH) -Wl,-Map,$(OUT)/$(NAME).map \
		$^ $(LIBPATHS) $(LIBS) -o $@

$(OUT)/$(NAME).nro: $(OUT)/$(NAME).elf $(OUT)/$(NAME).nacp $(ICON) $(STAMP)
	@find $(ROMFS) -name '.DS_Store' -delete
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --nacp=$(OUT)/$(NAME).nacp --icon=$(ICON) --romfsdir=$(ROMFS)

icon:
	python3 tools/make_icon.py

assets:
	python3 tools/prepare_assets.py

clean:
	rm -rf $(OUT) $(NAME).nro $(NAME).elf $(NAME).map

-include $(DEPS)
