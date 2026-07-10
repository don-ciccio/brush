CC = gcc
# -MMD -MP: per-object .d dependency files so header changes recompile every
# source that includes them (stale objects with mismatched struct layouts
# otherwise link together and corrupt memory at runtime).
CFLAGS = -Wall -Wextra -std=c11 -O2 -MMD -MP -Iengine

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LIBS = -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
endif
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -I/opt/homebrew/opt/raylib/include
    LIBS = -L/opt/homebrew/opt/raylib/lib -lraylib -lm \
           -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
endif

BUILD_DIR = build
ENGINE_LIB = $(BUILD_DIR)/libbrush.a
SANDBOX = $(BUILD_DIR)/sandbox

ENGINE_SRC = $(wildcard engine/*.c)
ENGINE_OBJ = $(ENGINE_SRC:engine/%.c=$(BUILD_DIR)/engine_%.o)
DEPS = $(ENGINE_OBJ:.o=.d) $(BUILD_DIR)/sandbox_main.d

all: $(SANDBOX)

# Engine builds as a static library; games link it and include engine/brush.h.
$(ENGINE_LIB): $(ENGINE_OBJ)
	ar rcs $@ $^

$(BUILD_DIR)/engine_%.o: engine/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sandbox_main.o: sandbox/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SANDBOX): $(BUILD_DIR)/sandbox_main.o $(ENGINE_LIB)
	$(CC) $^ -o $@ $(LIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(SANDBOX)
	./$(SANDBOX)

# Automated visual check: renders 180 frames, saves screenshot.png, exits.
verify: $(SANDBOX)
	BRUSH_AUTO_SCREENSHOT=1 ./$(SANDBOX)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

.PHONY: all run verify clean rebuild

-include $(DEPS)
