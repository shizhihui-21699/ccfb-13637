CXX      ?= clang++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -MMD -MP
PKG      := pkg-config
FF_CFLAGS := $(shell $(PKG) --cflags libavformat libavcodec libavutil libswscale)
FF_LIBS   := $(shell $(PKG) --libs   libavformat libavcodec libavutil libswscale)

SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
BIN := build/tamper-detect

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(FF_LIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(FF_CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

-include $(DEP)

.PHONY: all clean
