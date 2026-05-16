# ENux init Makefile
#
# Produces a statically linked ELF64 binary: build/init
#
# Build targets:
#   make          → build/init (release, -O2, stripped)
#   make debug    → build/init.debug (unstripped, -g, ASan)
#   make clean    → remove build artefacts
#   make install  → copy to /bedrock/libexec/enux-init (needs root)
#   make check    → run basic smoke tests
#
# Requirements:
#   gcc   (or clang — see CC override below)
#   nasm  (for the .asm file)
#   ld    (binutils)

# ── Toolchain ──────────────────────────────────────────────
CC      ?= gcc
NASM    ?= nasm
LD      ?= ld
STRIP   ?= strip
INSTALL ?= install

# ── Directories ────────────────────────────────────────────
SRCDIR   := src
ASMDIR   := asm
INCDIR   := include
BUILDDIR := build

# ── Sources ────────────────────────────────────────────────
C_SRCS := \
	$(SRCDIR)/main.c    \
	$(SRCDIR)/env.c     \
	$(SRCDIR)/mount.c   \
	$(SRCDIR)/stratum.c \
	$(SRCDIR)/menu.c    \
	$(SRCDIR)/conf.c    \
	$(SRCDIR)/util.c

ASM_SRCS := \
	$(ASMDIR)/asm_util.asm

# ── Object files ───────────────────────────────────────────
C_OBJS   := $(patsubst $(SRCDIR)/%.c,   $(BUILDDIR)/%.o,   $(C_SRCS))
ASM_OBJS := $(patsubst $(ASMDIR)/%.asm, $(BUILDDIR)/%.asm.o, $(ASM_SRCS))
ALL_OBJS := $(C_OBJS) $(ASM_OBJS)

# ── Flags ──────────────────────────────────────────────────

# Target: x86_64 Linux only (ENux is x86_64 for now)
ARCH_FLAGS := -m64 -march=x86-64 -mtune=generic

# C flags shared by all builds
CFLAGS_COMMON := \
	$(ARCH_FLAGS)          \
	-std=c11               \
	-I$(INCDIR)            \
	-Wall                  \
	-Wextra                \
	-Wpedantic             \
	-Wformat=2             \
	-Wstrict-prototypes    \
	-Wmissing-prototypes   \
	-fstack-protector-strong \
	-D_GNU_SOURCE          \
	-DENUX_VERSION=\"$(shell git describe --tags --always 2>/dev/null || echo dev)\"

# Release flags
CFLAGS_RELEASE := $(CFLAGS_COMMON) \
	-O2                    \
	-DNDEBUG               \
	-ffunction-sections    \
	-fdata-sections

# Debug flags
CFLAGS_DEBUG := $(CFLAGS_COMMON) \
	-O0                    \
	-g3                    \
	-DDEBUG                \
	-fsanitize=address,undefined \
	-fno-omit-frame-pointer

# NASM flags
# -f elf64  : ELF64 output (x86_64 Linux)
# -w+all    : enable all warnings
NASMFLAGS := -f elf64 -w+all

# Linker flags
# -static        : fully static binary (no glibc .so dependency)
#                  Critical for init — we run before any ld.so setup.
# --gc-sections  : dead-strip unused sections (pairs with -ffunction-sections)
# -z noexecstack : mark stack non-executable (security hardening)
LDFLAGS := \
	-static                \
	-Wl,--gc-sections      \
	-Wl,-z,noexecstack     \
	-Wl,--build-id=sha1

# Libraries needed (static versions)
# libattr: for getxattr/setxattr
# libc:    implicit with -static
LIBS := -lattr

# ── Default target ─────────────────────────────────────────
.PHONY: all
all: $(BUILDDIR)/init

# ── Release build ──────────────────────────────────────────
$(BUILDDIR)/init: CFLAGS := $(CFLAGS_RELEASE)
$(BUILDDIR)/init: $(ALL_OBJS) | $(BUILDDIR)
	$(CC) $(ARCH_FLAGS) $(LDFLAGS) -o $@ $(ALL_OBJS) $(LIBS)
	$(STRIP) --strip-all $@
	@echo ""
	@echo "  Built: $@"
	@ls -lh $@
	@echo ""

# ── Debug build ────────────────────────────────────────────
.PHONY: debug
debug: CFLAGS := $(CFLAGS_DEBUG)
debug: LDFLAGS_EXTRA := -fsanitize=address,undefined
debug: $(ALL_OBJS) | $(BUILDDIR)
	$(CC) $(ARCH_FLAGS) $(LDFLAGS) $(LDFLAGS_EXTRA) -o $(BUILDDIR)/init.debug $(ALL_OBJS) $(LIBS)
	@echo "  Built: $(BUILDDIR)/init.debug (debug + ASan)"

# ── Compile C sources ──────────────────────────────────────
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Assemble ASM sources ───────────────────────────────────
# NASM → ELF64 object, then the C compiler links it in normally.
$(BUILDDIR)/%.asm.o: $(ASMDIR)/%.asm | $(BUILDDIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# ── Build directory ────────────────────────────────────────
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Install ────────────────────────────────────────────────
INSTALL_PATH ?= /bedrock/libexec/enux-init

.PHONY: install
install: $(BUILDDIR)/init
	$(INSTALL) -D -m 0755 $< $(INSTALL_PATH)
	@echo "  Installed to $(INSTALL_PATH)"
	@echo "  You may now symlink /sbin/init → $(INSTALL_PATH)"

# ── Smoke tests ────────────────────────────────────────────
# These run on the build host (not in a Bedrock environment).
# They test that the binary is sane before installation.
.PHONY: check
check: $(BUILDDIR)/init
	@echo "── Check: ELF type ──────────────────────────────"
	@file $(BUILDDIR)/init | grep -q "ELF 64-bit" && echo "  OK: ELF64" || (echo "FAIL: not ELF64"; exit 1)
	@file $(BUILDDIR)/init | grep -q "statically linked" && echo "  OK: static" || (echo "FAIL: not static"; exit 1)
	@echo "── Check: no forbidden symbols ──────────────────"
	@# Ensure we're not accidentally pulling in dynamic libc calls that
	@# would fail before ld.so is set up
	@nm $(BUILDDIR)/init 2>/dev/null | grep -v "asm_" | grep -qE "system\b|popen\b" \
		&& (echo "FAIL: found system()/popen() — remove them"; exit 1) \
		|| echo "  OK: no system()/popen()"
	@echo "── Check: ASM symbols present ───────────────────"
	@nm $(BUILDDIR)/init | grep -q "asm_memzero"  && echo "  OK: asm_memzero" || echo "WARN: asm_memzero missing"
	@nm $(BUILDDIR)/init | grep -q "asm_strcmp"   && echo "  OK: asm_strcmp"  || echo "WARN: asm_strcmp missing"
	@nm $(BUILDDIR)/init | grep -q "asm_write_str" && echo "  OK: asm_write_str" || echo "WARN: asm_write_str missing"
	@echo "── Check: binary size ───────────────────────────"
	@ls -lh $(BUILDDIR)/init
	@echo ""
	@echo "All checks passed."

# ── Size report ────────────────────────────────────────────
.PHONY: size
size: $(BUILDDIR)/init
	@echo "── Section sizes ────────────────────────────────"
	@size $(BUILDDIR)/init
	@echo "── Per-object contribution ──────────────────────"
	@size $(ALL_OBJS)

# ── Disassembly (useful for inspecting ASM output) ─────────
.PHONY: disasm
disasm: $(BUILDDIR)/init
	objdump -d -M intel $(BUILDDIR)/init | less

# ── Clean ──────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

# ── Dependency tracking ────────────────────────────────────
# Auto-generate .d files so header changes trigger recompilation.
DEPFLAGS = -MT $@ -MMD -MP -MF $(BUILDDIR)/$*.d
$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(BUILDDIR)/%.d | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

DEPFILES := $(C_OBJS:.o=.d)
$(DEPFILES):
-include $(DEPFILES)
