CC = gcc
CXX = g++
# -MMD -MP: per-object .d dependency files so header changes recompile every
# source that includes them (stale objects with mismatched struct layouts
# otherwise link together and corrupt memory at runtime).
CFLAGS = -Wall -Wextra -std=c11 -O2 -MMD -MP -Iengine -Iexternal/joltc/include
CXXFLAGS = -Wall -Wextra -std=c++17 -O2 -MMD -MP -Iengine -Iexternal/joltc/include \
           -Iexternal/imgui -Iexternal/rlimgui -Iexternal/imguizmo/src

UNAME_S := $(shell uname -s)
JOLT_LIBS = -Lexternal/joltc/build/lib -ljoltc -lJolt
ifeq ($(UNAME_S),Linux)
    LIBS = $(JOLT_LIBS) -lstdc++ -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
endif
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -I/opt/homebrew/opt/raylib/include
    CXXFLAGS += -I/opt/homebrew/opt/raylib/include
    LIBS = -L/opt/homebrew/opt/raylib/lib $(JOLT_LIBS) -lc++ -lraylib -lm \
           -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
endif

BUILD_DIR = build
ENGINE_LIB = $(BUILD_DIR)/libbrush.a
SANDBOX = $(BUILD_DIR)/sandbox
EDITOR = $(BUILD_DIR)/editor

ENGINE_SRC = $(wildcard engine/*.c)
ENGINE_OBJ = $(ENGINE_SRC:engine/%.c=$(BUILD_DIR)/engine_%.o)

IMGUI_CORE_OBJ = $(BUILD_DIR)/imgui_core_imgui.o \
                 $(BUILD_DIR)/imgui_core_imgui_draw.o \
                 $(BUILD_DIR)/imgui_core_imgui_widgets.o \
                 $(BUILD_DIR)/imgui_core_imgui_tables.o \
                 $(BUILD_DIR)/imgui_core_imgui_demo.o
IMGUI_OBJS = $(IMGUI_CORE_OBJ) $(BUILD_DIR)/imgui_rlimgui.o $(BUILD_DIR)/imgui_imguizmo.o

EDITOR_SRC = $(wildcard editor/*.cpp)
EDITOR_OBJ = $(EDITOR_SRC:editor/%.cpp=$(BUILD_DIR)/editor_%.o)

DEPS = $(ENGINE_OBJ:.o=.d) $(BUILD_DIR)/sandbox_main.d $(IMGUI_OBJS:.o=.d) $(EDITOR_OBJ:.o=.d)

all: $(SANDBOX) $(EDITOR)

# Engine builds as a static library; games link it and include engine/brush.h.
$(ENGINE_LIB): $(ENGINE_OBJ)
	ar rcs $@ $^

$(BUILD_DIR)/engine_%.o: engine/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sandbox_main.o: sandbox/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SANDBOX): $(BUILD_DIR)/sandbox_main.o $(ENGINE_LIB)
	$(CC) $^ -o $@ $(LIBS)

# Compile ImGui Core
$(BUILD_DIR)/imgui_core_%.o: external/imgui/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile rlImGui
$(BUILD_DIR)/imgui_rlimgui.o: external/rlimgui/rlImGui.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile ImGuizmo
$(BUILD_DIR)/imgui_imguizmo.o: external/imguizmo/src/ImGuizmo.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile Editor translation units
$(BUILD_DIR)/editor_%.o: editor/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link Editor binary
$(EDITOR): $(EDITOR_OBJ) $(IMGUI_OBJS) $(ENGINE_LIB)
	$(CXX) $^ -o $@ $(LIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# One-time: build the vendored joltc (CMake fetches the Jolt sources).
deps:
	cmake -S external/joltc -B external/joltc/build -DCMAKE_BUILD_TYPE=Release \
	      -DJPH_BUILD_SHARED=OFF -DJPH_SAMPLES=OFF -DJPH_TESTS=OFF
	cmake --build external/joltc/build -j 8

run: $(SANDBOX)
	./$(SANDBOX)

# Run the editor
run-editor: $(EDITOR)
	./$(EDITOR)

# Automated visual check: renders 180 frames, saves screenshot.png, exits.
verify: $(SANDBOX)
	BRUSH_AUTO_SCREENSHOT=1 ./$(SANDBOX)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

.PHONY: all deps run run-editor verify clean rebuild

-include $(DEPS)
