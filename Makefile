HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = ORBIT.pdx

# Locate the SDK
SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif

ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

######
# IMPORTANT: You must add your source folders to VPATH for make to find them
######

VPATH += src
VPATH += cmark/src

# List C source files here - including cmark sources and clay layout
SRC = src/main.c \
      src/syscalls.c \
      src/clay_layout.c \
      cmark/src/blocks.c \
      cmark/src/buffer.c \
      cmark/src/cmark.c \
      cmark/src/cmark_ctype.c \
      cmark/src/commonmark.c \
      cmark/src/houdini_href_e.c \
      cmark/src/houdini_html_e.c \
      cmark/src/houdini_html_u.c \
      cmark/src/html.c \
      cmark/src/inlines.c \
      cmark/src/iterator.c \
      cmark/src/latex.c \
      cmark/src/man.c \
      cmark/src/node.c \
      cmark/src/references.c \
      cmark/src/render.c \
      cmark/src/scanners.c \
      cmark/src/utf8.c \
      cmark/src/xml.c

# List all user directories here (src first for our cmark config headers)
UINCDIR = src cmark/src

# List user asm files
UASRC =

# List all user C define here, like -D_DEBUG=1
# CLAY_DISABLE_SIMD: Playdate ARM doesn't have SSE/NEON we can use easily
UDEFS = -DCLAY_DISABLE_SIMD

# Define ASM defines here
UADEFS =

# List the user directory to look for the libraries here
ULIBDIR =

# List all user libraries here
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# Override simulator build to include UDEFS
$(OBJDIR)/pdex.${DYLIB_EXT}: $(SRC) | MKOBJDIR
	$(SIMCOMPILER) $(DYLIB_FLAGS) -lm -DTARGET_SIMULATOR=1 -DTARGET_EXTENSION=1 $(UDEFS) $(INCDIR) -o $(OBJDIR)/pdex.${DYLIB_EXT} $(SRC)
