# sim/Makefile — 码得 UI 模拟器（host 构建，无需 ESP-IDF）
#
# 用法：
#   make -C sim            # 编译（增量）
#   make -C sim run        # 编译并打开模拟器窗口
#   make -C sim shot OUT=out.bmp  # 截图模式
#
# 依赖：clang++（Xcode CLT）、SDL2（brew install sdl2）

ROOT  := ..
MADE  := $(ROOT)/made
LVGL  := $(MADE)/managed_components/lvgl__lvgl
COMP  := $(MADE)/components
CJSON := $(MADE)/managed_components/espressif__cjson/cJSON
FONTS := $(MADE)/managed_components/78__xiaozhi-fonts/src

BUILD := build
BIN   := $(BUILD)/sim

SYSROOT := /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk
CC  := clang
CXX := clang++

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || echo "-I/opt/homebrew/include")
PNG_CFLAGS := $(shell pkg-config --cflags libpng 2>/dev/null || echo "")
PNG_LIBS   := $(shell pkg-config --libs libpng 2>/dev/null || echo "-L/opt/homebrew/lib -lpng")
PNG_CFLAGS := $(shell pkg-config --cflags libpng 2>/dev/null || echo "")
PNG_LIBS   := $(shell pkg-config --libs libpng 2>/dev/null || echo "-L/opt/homebrew/lib -lpng")
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || echo "-L/opt/homebrew/lib -lSDL2")

WARN := -w
COMMON_INC := -I. -Ishims \
  -I$(LVGL) \
  -I$(COMP)/brookesia_app_vibe \
  -I$(COMP)/vibe_touch_keyboard \
  -I$(COMP)/vibe_i18n/include \
  -I$(COMP)/vibe_theme/include \
  -I$(COMP)/vibe_wifi/include \
  -I$(COMP)/vibe_pairing/include \
  -I$(COMP)/vibe_phone_setup/include \
  -I$(COMP)/vibe_usb/include \
  -I$(COMP)/vibe_voice_capture/include \
  -I$(COMP)/board_voice_audio/include \
  -I$(CJSON) \
  -I$(FONTS)

CFLAGS   := $(WARN) -isysroot $(SYSROOT) -O1 -g -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DLV_FONT_FMT_TXT_LARGE=1 $(COMMON_INC) $(SDL_CFLAGS) $(PNG_CFLAGS) $(PNG_CFLAGS)
CXXFLAGS := $(CFLAGS) -std=gnu++2b -fpermissive -include lv_compat.h
# 所有源码都用「仓库根相对路径」表达，目标文件平移到 build/obj/ 下，
# 避免目标路径里出现 ".."（沙箱/增量构建都不友好）。
LVGL_SRCS := $(patsubst $(ROOT)/%,%,$(shell find $(LVGL)/src -name '*.c'))
APP_SRCS  := \
  made/components/brookesia_app_vibe/esp_brookesia_app_vibe.cpp \
  made/components/brookesia_app_vibe/made_sprite.cpp \
  made/components/brookesia_app_vibe/assets/img_app_vibe.c \
  made/components/brookesia_app_vibe/assets/img_sprite_bean.c \
  made/components/vibe_touch_keyboard/vibe_touch_keyboard.cpp \
  made/components/vibe_i18n/vibe_i18n.cpp \
  made/components/vibe_theme/vibe_theme.cpp \
  made/managed_components/espressif__cjson/cJSON/cJSON.c \
  made/managed_components/78__xiaozhi-fonts/src/font_puhui_16_4.c \
  made/managed_components/78__xiaozhi-fonts/src/font_puhui_20_4.c \
  sim/stubs/esp_stubs.cpp \
  sim/stubs/components_stubs.cpp \
  sim/sim_main.cpp

SRCS := $(LVGL_SRCS) $(APP_SRCS)
OBJS := $(patsubst %,$(BUILD)/obj/%.o,$(SRCS))

.PHONY: all run clean shot
all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(SDL_LIBS) $(PNG_LIBS) $(PNG_LIBS)

$(BUILD)/obj/%.c.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

run: $(BIN)
	./$(BIN)

shot: $(BIN)
	./$(BIN) --shot $(OUT)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
