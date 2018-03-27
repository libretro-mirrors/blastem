ifndef OS
OS:=$(shell uname -s)
endif
FIXUP:=true

ifeq ($(OS),Windows)
ifndef SDL2_PREFIX
SDL2_PREFIX:="sdl/i686-w64-mingw32"
endif
ifndef GLEW_PREFIX
GLEW_PREFIX:=glew
endif
ifndef GLEW32S_LIB
GLEW32S_LIB:=$(GLEW_PREFIX)/lib/Release/Win32/glew32s.lib
endif

MEM:=mem_win.o
TERMINAL:=terminal_win.o
FONT:=nuklear_ui/font_win.o
NET:=net_win.o
EXE:=.exe
CC:=i686-w64-mingw32-gcc-win32
CFLAGS:=-std=gnu99 -Wreturn-type -Werror=return-type -Werror=implicit-function-declaration -I"$(SDL2_PREFIX)/include/SDL2" -I"$(GLEW_PREFIX)/include" -DGLEW_STATIC
LDFLAGS:= $(GLEW32S_LIB) -L"$(SDL2_PREFIX)/lib" -lm -lmingw32 -lSDL2main -lSDL2 -lws2_32 -lopengl32 -lglu32 -mwindows
CPU:=i686

else

MEM:=mem.o
TERMINAL:=terminal.o
NET:=net.o
EXE:=

ifeq ($(OS),Darwin)
LIBS=sdl2 glew
FONT:=nuklear_ui/font_mac.o
else
LIBS=sdl2 glew gl
FONT:=nuklear_ui/font.o
endif #Darwin

HAS_PROC:=$(shell if [ -d /proc ]; then /bin/echo -e -DHAS_PROC; fi)
CFLAGS:=-std=gnu99 -Wreturn-type -Werror=return-type -Werror=implicit-function-declaration -Wno-unused-value $(HAS_PROC) -DHAVE_UNISTD_H
ifeq ($(OS),Darwin)
#This should really be based on whether or not the C compiler is clang rather than based on the OS
CFLAGS+= -Wno-logical-op-parentheses
endif
ifdef PORTABLE
CFLAGS+= -DGLEW_STATIC -Iglew/include
LDFLAGS:=-lm glew/lib/libGLEW.a

ifeq ($(OS),Darwin)
CFLAGS+= -IFrameworks/SDL2.framework/Headers
LDFLAGS+= -FFrameworks -framework SDL2 -framework OpenGL -framework AppKit
FIXUP:=install_name_tool -change @rpath/SDL2.framework/Versions/A/SDL2 @executable_path/Frameworks/SDL2.framework/Versions/A/SDL2
else
CFLAGS+= -Isdl/include
LDFLAGS+= -Wl,-rpath='$$ORIGIN/lib' -Llib -lSDL2 $(shell pkg-config --libs gl)
endif #Darwin

else
CFLAGS:=$(shell pkg-config --cflags-only-I $(LIBS)) $(CFLAGS)
LDFLAGS:=-lm $(shell pkg-config --libs $(LIBS))

ifeq ($(OS),Darwin)
LDFLAGS+= -framework OpenGL -framework AppKit
endif

endif #PORTABLE
endif #Windows

ifdef DEBUG
ifeq ($(OS),Darwin)
OPT:=-g3 -O0
else
OPT:=-g3 -Og
endif #Darwin
else
ifdef NOLTO
OPT:=-O2
else
OPT:=-O2 -flto
endif #NOLTO
endif #DEBUG

CFLAGS:=$(OPT) $(CFLAGS)
LDFLAGS:=$(OPT) $(LDFLAGS)

ifdef Z80_LOG_ADDRESS
CFLAGS+= -DZ80_LOG_ADDRESS
endif

ifdef PROFILE
LDFLAGS+= -Wl,--no-as-needed -lprofiler -Wl,--as-needed
endif
ifdef NOGL
CFLAGS+= -DDISABLE_OPENGL
NONUKLEAR:=1
endif

ifdef M68030
CFLAGS+= -DM68030
endif
ifdef M68020
CFLAGS+= -DM68020
endif
ifdef M68010
CFLAGS+= -DM68010
endif

ifndef CPU
CPU:=$(shell uname -m)
endif

#OpenBSD uses different names for these architectures
ifeq ($(CPU),amd64)
CPU:=x86_64
else
ifeq ($(CPU),i386)
CPU:=i686
endif
endif

TRANSOBJS=gen.o backend.o $(MEM) arena.o tern.o
M68KOBJS=68kinst.o m68k_core.o
ifeq ($(CPU),x86_64)
M68KOBJS+= m68k_core_x86.o
TRANSOBJS+= gen_x86.o backend_x86.o
else
ifeq ($(CPU),i686)
M68KOBJS+= m68k_core_x86.o
TRANSOBJS+= gen_x86.o backend_x86.o
endif
endif

Z80OBJS=z80inst.o z80_to_x86.o
AUDIOOBJS=ym2612.o psg.o wave.o
CONFIGOBJS=config.o tern.o util.o paths.o 
NUKLEAROBJS=$(FONT) nuklear_ui/blastem_nuklear.o nuklear_ui/sfnt.o
RENDEROBJS=render_sdl.o ppm.o
LIBZOBJS=zlib/adler32.o zlib/compress.o zlib/crc32.o zlib/deflate.o zlib/gzclose.o zlib/gzlib.o zlib/gzread.o\
	zlib/gzwrite.o zlib/infback.o zlib/inffast.o zlib/inflate.o zlib/inftrees.o zlib/trees.o zlib/uncompr.o zlib/zutil.o
	
ifdef NOZLIB
CFLAGS+= -DDISABLE_ZLIB
else
RENDEROBJS+= $(LIBZOBJS) png.o
endif

MAINOBJS=blastem.o system.o genesis.o debug.o gdb_remote.o vdp.o $(RENDEROBJS) io.o romdb.o hash.o menu.o xband.o \
	realtec.o i2c.o nor.o sega_mapper.o multi_game.o megawifi.o $(NET) serialize.o $(TERMINAL) $(CONFIGOBJS) gst.o \
	$(M68KOBJS) $(TRANSOBJS) $(AUDIOOBJS) saves.o zip.o 
	
ifdef NONUKLEAR
CFLAGS+= -DDISABLE_NUKLEAR
else
MAINOBJS+= $(NUKLEAROBJS)
endif

ifeq ($(CPU),x86_64)
CFLAGS+=-DX86_64 -m64
LDFLAGS+=-m64
else
ifeq ($(CPU),i686)
CFLAGS+=-DX86_32 -m32
LDFLAGS+=-m32
else
$(error $(CPU) is not a supported architecture)
endif
endif

ifdef NOZ80
CFLAGS+=-DNO_Z80
else
MAINOBJS+= sms.o $(Z80OBJS)
endif

ifeq ($(OS),Windows)
MAINOBJS+= res.o
endif

ALL=dis$(EXE) zdis$(EXE) stateview$(EXE) vgmplay$(EXE) blastem$(EXE)
ifneq ($(OS),Windows)
ALL+= termhelper
endif

all : $(ALL)

blastem$(EXE) : $(MAINOBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	$(FIXUP) ./$@
	
blastjag$(EXE) : jaguar.o jag_video.o $(RENDEROBJS) serialize.o $(M68KOBJS) $(TRANSOBJS) $(CONFIGOBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

dis$(EXE) : dis.o 68kinst.o tern.o vos_program_module.o
	$(CC) -o $@ $^ $(OPT)
	
jagdis : jagdis.o jagcpu.o tern.o
	$(CC) -o $@ $^

zdis$(EXE) : zdis.o z80inst.o
	$(CC) -o $@ $^

libemu68k.a : $(M68KOBJS) $(TRANSOBJS)
	ar rcs libemu68k.a $(M68KOBJS) $(TRANSOBJS)

trans : trans.o serialize.o $(M68KOBJS) $(TRANSOBJS) util.o
	$(CC) -o trans trans.o $(M68KOBJS) $(TRANSOBJS) util.o $(OPT)

transz80 : transz80.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o transz80 transz80.o $(Z80OBJS) $(TRANSOBJS)

ztestrun : ztestrun.o serialize.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o ztestrun ztestrun.o $(Z80OBJS) $(TRANSOBJS) $(OPT)

ztestgen : ztestgen.o z80inst.o
	$(CC) -ggdb -o ztestgen ztestgen.o z80inst.o

stateview$(EXE) : stateview.o vdp.o $(RENDEROBJS) serialize.o $(CONFIGOBJS) gst.o
	$(CC) -o $@ $^ $(LDFLAGS)
	$(FIXUP) ./$@

vgmplay$(EXE) : vgmplay.o $(RENDEROBJS) serialize.o $(CONFIGOBJS) $(AUDIOOBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	$(FIXUP) ./$@

blastcpm : blastcpm.o util.o serialize.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o $@ $^ $(OPT)

test : test.o vdp.o
	$(CC) -o test test.o vdp.o

testgst : testgst.o gst.o
	$(CC) -o testgst testgst.o gst.o

test_x86 : test_x86.o gen_x86.o gen.o
	$(CC) -o test_x86 test_x86.o gen_x86.o gen.o

test_arm : test_arm.o gen_arm.o mem.o gen.o
	$(CC) -o test_arm test_arm.o gen_arm.o mem.o gen.o
	
test_int_timing : test_int_timing.o vdp.o
	$(CC) -o $@ $^

gen_fib : gen_fib.o gen_x86.o mem.o
	$(CC) -o gen_fib gen_fib.o gen_x86.o mem.o

offsets : offsets.c z80_to_x86.h m68k_core.h
	$(CC) -o offsets offsets.c

vos_prog_info : vos_prog_info.o vos_program_module.o
	$(CC) -o vos_prog_info vos_prog_info.o vos_program_module.o

%.o : %.S
	$(CC) -c -o $@ $<

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<
  
%.o : %.m
	$(CC) $(CFLAGS) -c -o $@ $<

%.png : %.xcf
	xcf2png $< > $@

%.tiles : %.spec
	./img2tiles.py -s $< $@

%.bin : %.s68
	vasmm68k_mot -Fbin -m68000 -no-opt -spaces -o $@ -L $@.list $<

%.bin : %.sz8
	vasmz80_mot -Fbin -spaces -o $@ $<
res.o : blastem.rc
	i686-w64-mingw32-windres blastem.rc res.o

arrow.tiles : arrow.png
cursor.tiles : cursor.png
font_interlace_variable.tiles : font_interlace_variable.png
button.tiles : button.png
font.tiles : font.png

menu.bin : font_interlace_variable.tiles arrow.tiles cursor.tiles button.tiles font.tiles

clean :
	rm -rf $(ALL) trans ztestrun ztestgen *.o nuklear_ui/*.o zlib/*.o
