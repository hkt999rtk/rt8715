.DEFAULT_GOAL=all
.PHONY: mp
mp: ram_lp ram_mp
.PHONY: all
all: ram_lp ram_is

OS := $(shell uname)
.PRECIOUS: %.o %.d %.i

# Parallel build: auto-detect CPU cores (fallback to 4)
MAKEFLAGS += --no-print-directory -j$(shell nproc 2>/dev/null || echo 4)
	
.PHONY: toolchain
toolchain:
ifeq ($(findstring CYGWIN, $(OS)), CYGWIN)
	if [ ! -d ../../../tools/arm-none-eabi-gcc/asdk ] ; then mkdir ../../../tools/arm-none-eabi-gcc/asdk; fi;\
	if [ ! -d ../../../tools/arm-none-eabi-gcc/asdk/cygwin ] ; then tar -jxf ../../../tools/arm-none-eabi-gcc/asdk-6.4.1-cygwin-newlib-build-2778-i686.tar.bz2 -C ../../../tools/arm-none-eabi-gcc/ ; mv ../../../tools/arm-none-eabi-gcc/asdk-6.4.1/cygwin ../../../tools/arm-none-eabi-gcc/asdk/cygwin ;fi
endif

ifeq ($(findstring MINGW32, $(OS)), MINGW32)
	if [ ! -f /bin/unzip.exe ] ; then mingw-get.exe install msys-unzip; fi;\
	if [ ! -d ../../../tools/arm-none-eabi-gcc/asdk ] ; then mkdir ../../../tools/arm-none-eabi-gcc/asdk; fi;\
	if [ ! -d ../../../tools/arm-none-eabi-gcc/asdk/mingw32 ] ; then unzip -q -u ../../../tools/arm-none-eabi-gcc/asdk-5.5.1-mingw32-newlib-build-2754.zip ; mv asdk-5.5.1/mingw32 ../../../tools/arm-none-eabi-gcc/asdk/mingw32 ; rm -rf asdk-5.5.1 ;fi
endif

ifeq ($(findstring Linux, $(OS)), Linux)
	if [ ! -d ../../../tools/arm-none-eabi-gcc/asdk ] ; then mkdir ../../../tools/arm-none-eabi-gcc/asdk; fi;\
	if [ ! -d ../../../tools/arm-none-eabi-gcc/asdk/linux ] ; then tar -jxf ../../../tools/arm-none-eabi-gcc/asdk-6.4.1-linux-newlib-build-3026-x86_64.tar.bz2 -C ../../../tools/arm-none-eabi-gcc/ ; mv ../../../tools/arm-none-eabi-gcc/asdk-6.4.1/linux ../../../tools/arm-none-eabi-gcc/asdk/linux ; rmdir ../../../tools/arm-none-eabi-gcc/asdk-6.4.1; fi
endif

# ----- Incremental builds (only recompile changed files) -----
.PHONY: ram_is
ram_is: toolchain
	@$(MAKE) -f application.is.mk all

.PHONY: ram_mp
ram_mp: toolchain
	@$(MAKE) -f application.is.mk mp

.PHONY: ram_lp
ram_lp: toolchain
	@$(MAKE) -f application.lp.mk all

# ----- Full rebuilds (clean + build) -----
.PHONY: rebuild_is
rebuild_is: toolchain
	@$(MAKE) -f application.is.mk clean all

.PHONY: rebuild_mp
rebuild_mp: toolchain
	@$(MAKE) -f application.is.mk clean mp

.PHONY: rebuild_lp
rebuild_lp: toolchain
	@$(MAKE) -f application.lp.mk clean all

.PHONY: clean
clean:
	@$(MAKE) -f application.lp.mk clean
	@$(MAKE) -f application.is.mk clean

.PHONY: clean_lp
clean_lp:
	@$(MAKE) -f application.lp.mk clean
	
.PHONY: clean_is
clean_is:
	@$(MAKE) -f application.is.mk clean


.PHONY: flash debug setup
setup:
	@$(MAKE) -f application.is.mk $(MAKECMDGOALS)

flash: toolchain
	@$(MAKE) -f application.is.mk flashburn

debug: toolchain
	@$(MAKE) -f application.is.mk debug

# backtrace parser:
#   make backtrace ADDR="..."   (map mode, all symbols)
#   make symbols ADDR="..."     (nm function-only, faster)
#   make text ADDR="980c55f5"   (addr2line, precise file:line)
.PHONY: backtrace symbols text
backtrace:
	@$(MAKE) -f application.is.mk backtrace ADDR="$(ADDR)"

symbols:
	@$(MAKE) -f application.is.mk symbols ADDR="$(ADDR)"

text:
	@$(MAKE) -f application.is.mk text ADDR="$(ADDR)"
