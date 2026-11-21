DEVKITPRO ?= /opt/devkitpro
LIBOUNCE := $(DEVKITPRO)/libounce
PORTLIBS := $(DEVKITPRO)/portlibs/switch
PORTLIBS_SWITCH2 := $(DEVKITPRO)/portlibs/switch2
TOOLS := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-

include $(LIBOUNCE)/switch2_cpu.mk

NAME := junkbot
OTR_OUT ?= junkbot.otr
TITLE := LEGO Junkbot
AUTHOR := mysterypaint, 1j01
VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_PATCH := 0
VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
RELEASE_VERSION := v$(VERSION_MAJOR).$(VERSION_MINOR)
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

.PHONY: all clean assets icon otr otr-code otr-dir release print-version

# Multiple phony goals (notably `make clean all -jN`) otherwise race with the
# generated-header build and can link before every object exists.
.NOTPARALLEL:

# `make clean all -jN` must not delete build outputs while all is building.
ifneq ($(filter clean,$(MAKECMDGOALS)),)
ifneq ($(filter-out clean,$(MAKECMDGOALS)),)
$(filter-out clean,$(MAKECMDGOALS)): | clean
endif
endif

all: $(OUT)/$(NAME).nro

$(STAMP): Makefile tools/prepare_code_assets.py tools/builtin_palettes.py \
		tools/data/sprite_recipes.json tools/data/bitmap_points.json \
		tools/data/level_listing.txt tools/data/junkbot_offsets.json \
		tools/data/Title\ Screen.txt assets/04B_08__.TTF
	python3 tools/prepare_code_assets.py
	python3 tools/make_icon.py
	@mkdir -p $(OUT)
	@touch $@

$(OBJECTS): | $(STAMP)

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
	python3 tools/prepare_code_assets.py

otr:
	python3 tools/make_otr.py $(OTR_ARGS)

otr-code:
	@test -n "$(JUNKBOT_CODE)" || (echo "set JUNKBOT_CODE=/path/to/junkbot-code" && exit 1)
	python3 tools/make_otr.py --code "$(JUNKBOT_CODE)" -o $(OTR_OUT)

otr-dir:
	@test -n "$(JUNKBOT_DIR)" || (echo "set JUNKBOT_DIR=/path/to/junkbot.dir" && exit 1)
	python3 tools/make_otr.py --dir "$(JUNKBOT_DIR)" -o $(OTR_OUT)

print-version:
	@printf '%s\n' '$(RELEASE_VERSION)'

release: all
	python3 tools/make_release.py "$(RELEASE_VERSION)"

clean:
	rm -rf $(OUT) $(NAME).nro $(NAME).elf $(NAME).map

-include $(DEPS)
