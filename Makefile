# init.c Makefile
#
# Produces a statically linked ELF64 binary: build/init
#
# Build targets:
#   make            → build/init (release, -O2, stripped)
#   make DEBUG=1    → build/init.debug (unstripped, -g3, ASan/UBSan)
#   make debug      → alias for DEBUG=1
#   make check      → basic smoke tests on the binary
#   make install    → install to /bedrock/strata/bedrock/sbin/enux-init
#                     (never touches the original Bedrock init)
#   make clean      → remove build artefacts
#
# Requirements: gcc (or clang), nasm, binutils.

CC      ?= gcc
NASM    ?= nasm
STRIP   ?= strip
INSTALL ?= install

SRCDIR   := src
ASMDIR   := asm
INCDIR   := include
ifeq ($(DEBUG),1)
BUILDDIR := build/debug
else
BUILDDIR := build
endif

C_SRCS := \
	$(SRCDIR)/main.c         \
	$(SRCDIR)/env.c          \
	$(SRCDIR)/mount.c        \
	$(SRCDIR)/stratum.c      \
	$(SRCDIR)/menu.c         \
	$(SRCDIR)/hijack.c       \
	$(SRCDIR)/bedrock_conf.c \
	$(SRCDIR)/brl.c          \
	$(SRCDIR)/util.c

ASM_SRCS := \
	$(ASMDIR)/asm_util.asm

C_OBJS   := $(patsubst $(SRCDIR)/%.c,   $(BUILDDIR)/%.o,     $(C_SRCS))
ASM_OBJS := $(patsubst $(ASMDIR)/%.asm, $(BUILDDIR)/%.asm.o, $(ASM_SRCS))
ALL_OBJS := $(C_OBJS) $(ASM_OBJS)

# Target: x86_64 Linux only
ARCH_FLAGS := -m64 -march=x86-64 -mtune=generic

CFLAGS_COMMON := \
	$(ARCH_FLAGS)            \
	-std=c11                 \
	-I$(INCDIR)              \
	-Wall                    \
	-Wextra                  \
	-Werror                  \
	-Wpedantic               \
	-Wformat=2               \
	-Wstrict-prototypes      \
	-Wmissing-prototypes     \
	-fstack-protector-strong \
	-D_GNU_SOURCE            \
	-DENUX_VERSION=\"$(shell git describe --tags --always 2>/dev/null || echo dev)\"

# NASM flags: ELF64 output, all warnings
NASMFLAGS := -f elf64 -w+all

ifeq ($(DEBUG),1)
# ASan cannot be linked statically; the debug binary is for host-side
# testing, never for booting.
CFLAGS  := $(CFLAGS_COMMON) -O0 -g3 -DDEBUG \
	-fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined \
	-Wl,-z,noexecstack
BIN     := $(BUILDDIR)/init.debug
else
# -static       : no libc.so dependency — we run before ld.so is usable
# -z noexecstack: non-executable stack
CFLAGS  := $(CFLAGS_COMMON) -O2 -DNDEBUG \
	-ffunction-sections -fdata-sections
LDFLAGS := \
	-static                \
	-Wl,--gc-sections      \
	-Wl,-z,noexecstack     \
	-Wl,--build-id=sha1
BIN     := $(BUILDDIR)/init
endif

.PHONY: all
all: $(BIN)

$(BIN): $(ALL_OBJS) | $(BUILDDIR)
	$(CC) $(ARCH_FLAGS) $(LDFLAGS) -o $@ $(ALL_OBJS)
ifneq ($(DEBUG),1)
	$(STRIP) --strip-all $@
endif
	@echo ""
	@echo "  Built: $@"
	@ls -lh $@
	@echo ""

.PHONY: debug
debug:
	$(MAKE) DEBUG=1

# Compile C (with auto-generated header dependencies)
DEPFLAGS = -MT $@ -MMD -MP -MF $(BUILDDIR)/$*.d
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Assemble NASM → ELF64 object
$(BUILDDIR)/%.asm.o: $(ASMDIR)/%.asm | $(BUILDDIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Install
#
# The binary is installed ALONGSIDE the original Bedrock init, never over
# it.  /bedrock/strata/bedrock/sbin/init must stay intact: it is the
# dual-boot selector's option 2 and the pre-pivot fallback path — always.
#
# During install, the original init is backed up as init.sh.bak (shell
# script) or init.bin.bak (binary) so the fallback/NOT_PID1 paths in the
# C code can find it.
# See INSTALL.md for activation (kernel init= parameter) and rollback.
INSTALL_PATH      ?= /bedrock/strata/bedrock/sbin/init
ORIGINAL_INIT_PATH = /bedrock/strata/bedrock/sbin/init

# Create a .sh.bak or .bin.bak backup of the original init, depending
# on whether it is a shell script or binary.
define do_backup
	if [ -f $(ORIGINAL_INIT_PATH) ] && [ ! "$(ORIGINAL_INIT_PATH)" -ef "$(INSTALL_PATH)" ]; then \
		if head -c 2 "$(ORIGINAL_INIT_PATH)" | grep -q "^#!"; then \
			ext=".sh.bak"; \
		else \
			ext=".bin.bak"; \
		fi; \
		$(INSTALL) -m 0755 "$(ORIGINAL_INIT_PATH)" "$(ORIGINAL_INIT_PATH)$$ext"; \
		echo "  Backed up: $(ORIGINAL_INIT_PATH) -> $(ORIGINAL_INIT_PATH)$$ext"; \
	else \
		echo "  No init to back up at $(ORIGINAL_INIT_PATH)"; \
	fi
endef

.PHONY: install
install: $(BUILDDIR)/init
	$(do_backup)
	$(INSTALL) -D -m 0755 $(BUILDDIR)/init $(INSTALL_PATH)
	@echo "  Installed: $(INSTALL_PATH)"
	@echo "  Activate via kernel parameter: init=$(INSTALL_PATH)"
	@echo "  See INSTALL.md for GRUB setup and rollback."

# Download the official Bedrock Linux shell init for environments where
# there was no existing init to back up, or a fresh copy is wanted.
# Saves it as both the .sh.bak backup and /sbin/init recovery fallback.
.PHONY: install-fallback
install-fallback:
	@echo "  Fetching official Bedrock Linux shell init ..."
	curl -sSL -o /tmp/bedrock-init \
		"https://raw.githubusercontent.com/bedrocklinux/bedrocklinux/refs/heads/master/src/bedrock/usr/libexec/bedrock-init" || \
		{ echo "FAIL: download failed"; exit 1; }
	chmod 755 /tmp/bedrock-init
	$(INSTALL) -D -m 0755 /tmp/bedrock-init $(ORIGINAL_INIT_PATH).sh.bak
	echo "  Saved: $(ORIGINAL_INIT_PATH).sh.bak"
	$(INSTALL) -D -m 0755 /tmp/bedrock-init /sbin/init
	echo "  Saved: /sbin/init (recovery fallback)"
	rm -f /tmp/bedrock-init

# Smoke tests (run on the build host, not a Bedrock environment)
.PHONY: check
check: $(BUILDDIR)/init
	@echo "Check: ELF type"
	@file $(BUILDDIR)/init | grep -q "ELF 64-bit" && echo "  OK: ELF64" || (echo "FAIL: not ELF64"; exit 1)
	@file $(BUILDDIR)/init | grep -q "statically linked" && echo "  OK: static" || (echo "FAIL: not static"; exit 1)
	@echo "Check: no forbidden symbols (in objects; final binary is stripped)"
	@nm $(C_OBJS) 2>/dev/null | grep -qE " U (system|popen|gets|strcpy|sprintf)$$" \
		&& (echo "FAIL: forbidden libc call referenced"; exit 1) \
		|| echo "  OK: no system/popen/gets/strcpy/sprintf"
	@echo "Check: ASM symbols present"
	@for sym in asm_memzero asm_strcmp asm_write_str asm_poll_read asm_usleep; do \
		nm $(ASM_OBJS) | grep -q "T $$sym" && echo "  OK: $$sym" || (echo "FAIL: $$sym missing"; exit 1); \
	done
	@echo "Check: binary size"
	@ls -lh $(BUILDDIR)/init
	@echo ""
	@echo "All checks passed."

.PHONY: size
size: $(BIN)
	@size $(BIN)
	@size $(ALL_OBJS)

.PHONY: disasm
disasm: $(BUILDDIR)/init
	objdump -d -M intel $(BUILDDIR)/init | less

.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

DEPFILES := $(C_OBJS:.o=.d)
$(DEPFILES):
-include $(DEPFILES)
