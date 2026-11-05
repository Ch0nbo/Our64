# Our64 crt-preview (PC): the TV-only shaders drawn headless. See crt_preview.cpp.
crt-preview: crt_preview.cpp ../../player/source/tv_shaders.h
	$(CXX) -O1 -g -std=c++17 -Wall -o $@ crt_preview.cpp -lEGL -lOpenGL
clean:
	rm -f crt-preview
