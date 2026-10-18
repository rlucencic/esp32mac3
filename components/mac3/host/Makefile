# Host build of the Macintosh LC II / LC III core: make -C components/mac3/host
#   build/mac_host <rom> [frames] [out.ppm] [ram_mb]   (see host_main.c)
MUSASHI   := ../../../../esp32emu/components/musashi
EMUDBG    := ../../../../esp32emu/components/emudbg
MAC       := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -DM68K_68030 -DM68K_INSTRUCTION_HOOK=2 -I$(MAC)/include -I$(MAC) -I$(MUSASHI) -I$(MUSASHI)/musashi -I$(MUSASHI)/generated -I$(EMUDBG)/include

MAC_SRCS  := $(wildcard $(MAC)/*.c)
M68K_SRCS := $(MUSASHI)/musashi/m68kcpu.c $(MUSASHI)/musashi/m68kdasm.c \
             $(MUSASHI)/musashi/softfloat/softfloat.c $(MUSASHI)/generated/m68kops.c \
             $(MUSASHI)/m68k_dbg.c
MAC_OBJS  := $(patsubst $(MAC)/%.c,$(BUILD)/mac/%.o,$(MAC_SRCS))
M68K_OBJS := $(patsubst $(MUSASHI)/%.c,$(BUILD)/m68k/%.o,$(M68K_SRCS)) \
             $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o

$(BUILD)/mac_host: host_main.c $(MAC_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/mac/%.o: $(MAC)/%.c $(wildcard $(MAC)/*.h $(MAC)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m68k/%.o: $(MUSASHI)/%.c $(MUSASHI)/m68kconf.h $(EMUDBG)/include/emudbg.h $(wildcard $(MUSASHI)/musashi/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: clean
