# PC tests for the player's portable parts. make -C tests
CC	?=	cc
CFLAGS	:=	-std=gnu11 -O1 -g -Wall -Wextra -I../player/source
BUILD	:=	build
TESTS	:=	test_saves test_states test_input test_prompt test_notices test_pause test_library test_setup test_tpak test_rsp_lle test_gb_tower

.PHONY: all clean shots
all: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do echo "$$t:"; $(BUILD)/$$t || exit 1; done

$(BUILD)/test_saves: test_saves.c ../player/source/saves.c ../player/source/saves.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_saves.c ../player/source/saves.c

$(BUILD)/test_states: test_states.c ../player/source/states.c ../player/source/states.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_states.c ../player/source/states.c -lz

$(BUILD)/test_input: test_input.c ../player/source/input.c ../player/source/input.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_input.c ../player/source/input.c -lm

$(BUILD)/test_prompt: test_prompt.c ../player/source/prompt.c ../player/source/prompt.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_prompt.c ../player/source/prompt.c

$(BUILD)/test_notices: test_notices.c ../player/source/notices.c ../player/source/notices.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_notices.c ../player/source/notices.c

$(BUILD)/test_pause: test_pause.c ../player/source/pause.c ../player/source/pause.h ../player/source/prompt.c ../player/source/states.h ../player/source/theme.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_pause.c ../player/source/pause.c ../player/source/prompt.c ../player/source/theme.c -lm

$(BUILD)/test_library: test_library.c ../player/source/library.c ../player/source/library.h ../player/source/prompt.c ../player/source/saves.c ../player/source/gamedb.c ../player/source/theme.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_library.c ../player/source/library.c ../player/source/prompt.c ../player/source/saves.c ../player/source/gamedb.c ../player/source/theme.c -lm

$(BUILD)/test_setup: test_setup.c ../player/source/setup.c ../player/source/setup.h ../player/source/input.c ../player/source/prompt.c ../player/source/pause.c ../player/source/theme.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_setup.c ../player/source/setup.c ../player/source/input.c ../player/source/prompt.c ../player/source/pause.c ../player/source/theme.c -lm

$(BUILD)/test_tpak: test_tpak.c ../player/source/tpak.c ../player/source/tpak.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../third_party/minilzo -o $@ test_tpak.c ../player/source/tpak.c ../third_party/minilzo/minilzo.c

# cxd4 (vendored) at -O2, as in the player; its own warnings off.
CXD4	:=	../third_party/cxd4
CXD4_SRC :=	su.c vu/vu.c vu/multiply.c vu/add.c vu/select.c vu/logical.c vu/divide.c
$(BUILD)/test_rsp_lle: test_rsp_lle.c ../player/source/rsp_lle.c ../player/source/rsp_lle.h $(addprefix $(CXD4)/,$(CXD4_SRC))
	@mkdir -p $(BUILD)/cxd4/vu
	@for f in $(CXD4_SRC); do $(CC) -std=gnu11 -O2 -g -w -c $(CXD4)/$$f -o $(BUILD)/cxd4/$${f%.c}.o || exit 1; done
	$(CC) $(CFLAGS) -O2 -I$(CXD4) -o $@ test_rsp_lle.c ../player/source/rsp_lle.c $(addprefix $(BUILD)/cxd4/,$(CXD4_SRC:.c=.o))

# The GB Tower's program in C against cxd4 (built by the rule above).
$(BUILD)/test_gb_tower: test_gb_tower.c ../player/source/gb_tower_hle.c ../player/source/gb_tower_hle.h $(BUILD)/test_rsp_lle
	$(CC) $(CFLAGS) -O2 -I$(CXD4) -o $@ test_gb_tower.c ../player/source/gb_tower_hle.c ../player/source/rsp_lle.c \
		$(addprefix $(BUILD)/cxd4/,$(CXD4_SRC:.c=.o))

# Every screen drawn on the PC with the console's drawing code and font: results/shots.
LIBGUI	:=	../third_party/libgui/source
SHOTS_GUI :=	libgui/GuiGeometry libgui/GuiPaint libgui/GuiTextRenderer libgui/GuiMeshCache drivers/Mutex
SHOTS_C	:=	pause.c prompt.c setup.c input.c library.c notices.c saves.c gamedb.c states.c theme.c
SHOTS_FLAGS :=	-O1 -g -I../player/source -I$(LIBGUI) -I$(LIBGUI)/libgui $(shell pkg-config --cflags freetype2 libpng)
$(BUILD)/ui_shots: ui_shots.cpp ../player/source/ui_draw.cpp ../player/source/ui_draw.h ../player/source/ui.h \
		$(addprefix ../player/source/,$(SHOTS_C)) $(addprefix $(LIBGUI)/,$(addsuffix .cpp,$(SHOTS_GUI)))
	@mkdir -p $(BUILD)/shots
	@for f in $(SHOTS_C); do $(CC) -std=gnu11 $(SHOTS_FLAGS) -w -c ../player/source/$$f -o $(BUILD)/shots/$${f%.c}.o || exit 1; done
	@for f in $(SHOTS_GUI); do o=$(BUILD)/shots/$$(basename $$f).o; [ $$o -nt $(LIBGUI)/$$f.cpp ] || \
		$(CXX) -std=gnu++17 $(SHOTS_FLAGS) -w -c $(LIBGUI)/$$f.cpp -o $$o || exit 1; done
	$(CXX) -std=gnu++17 $(SHOTS_FLAGS) -Wall -o $@ ui_shots.cpp ../player/source/ui_draw.cpp $(BUILD)/shots/*.o \
		$(shell pkg-config --libs freetype2 libpng libturbojpeg) -lz -lm

shots: $(BUILD)/ui_shots
	$(BUILD)/ui_shots
	$(BUILD)/ui_shots --theme comrade --out ../results/shots/comrade

clean:
	rm -rf $(BUILD)
