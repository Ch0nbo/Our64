# Our64 crt-preview (PC): the TV-only shaders drawn headless. See crt_preview.cpp.
all: crt-preview ra-preview
ra-preview: ra_preview.cpp
	$(CXX) -O1 -g -std=c++17 -Wall -o $@ ra_preview.cpp -lEGL -lOpenGL
crt-preview: crt_preview.cpp ../../player/source/tv_shaders.h ../../player/source/tv_mesh.h
	$(CXX) -O1 -g -std=c++17 -Wall -o $@ crt_preview.cpp -lEGL -lOpenGL
clean:
	rm -f crt-preview ra-preview
