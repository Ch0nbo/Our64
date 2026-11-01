# Our64 replay64 (PC): plays .o64f captures through a mupen64plus video plugin.
# GLideN64, vendored; only its API headers are used here.
GLIDEN64 ?= $(CURDIR)/../../third_party/gliden64
CXXFLAGS := -O2 -g -std=c++17 -Wall -I$(GLIDEN64)/src/inc

all: replay64 replay_core.so

replay64: replay64.cpp ../../replay/replay_engine.h ../../bench/source/capture.h
	$(CXX) $(CXXFLAGS) $< -o $@ -lz -ldl

replay_core.so: replay_core.cpp ../../replay/core_config.cpp
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags sdl2) -shared -fPIC $^ -o $@ $(shell pkg-config --libs sdl2)

clean:
	rm -f replay64 replay_core.so
