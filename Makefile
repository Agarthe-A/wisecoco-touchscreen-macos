# Build the Wisecoco touchscreen tools for macOS.
# Requires the Command Line Tools (xcode-select --install); no Xcode project.

CC         := clang
CFLAGS     := -O2 -Wall
FRAMEWORKS := -framework IOKit -framework CoreFoundation

BUILD := build
TOOLS := wisecoco-touch-enable wisecoco-touch-dump
BINS  := $(addprefix $(BUILD)/,$(TOOLS))

.PHONY: all clean

all: $(BINS)

$(BUILD)/%: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
