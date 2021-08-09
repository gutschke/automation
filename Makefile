# CXX    := clang++-6.0
CXX      := g++
CFLAGS   := --std=gnu++2a -g -Wall -D_DEFAULT_SOURCE -fno-rtti -fno-exceptions \
            -Wno-psabi -I libwebsockets/include
LFLAGS   := -Wall
LIBS     := -lfmt -lpugixml libwebsockets/lib/libwebsockets.a -lcap

all: automation
SRCS     := $(shell echo *.cpp)

ifneq (clean, $(filter clean, $(MAKECMDGOALS)))
  -include .build/debug
  -include $(patsubst %.cpp,.build/%.d,$(SRCS))
  ifneq ($(DEBUG),$(OLDDEBUG))
    override _ := $(shell $(MAKE) clean)
  endif
endif
ifeq (1,$(DEBUG))
  DFLAGS := -fsanitize=address -fno-omit-frame-pointer -O0
# DFLAGS := -fno-omit-frame-pointer -O0
  LFLAGS += -static-libasan
else
  DFLAGS := -O3
  CFLAGS += -DNDEBUG
  LFLAGS += -s
endif

.PHONY: clean
clean:
	rm -rf automation .build
	@[ "$(DEBUG)" = 1 ] && { mkdir -p .build; { echo 'DEBUG ?= 1'; echo 'override OLDDEBUG := 1'; } >.build/debug; } || :

automation: $(patsubst %.cpp,.build/%.o,$(SRCS)) .build/debug
	$(CXX) $(DFLAGS) $(LFLAGS) -o $@ $(patsubst %.cpp,.build/%.o,$(SRCS)) $(LIBS)

.build/%.o: %.cpp | .build/debug
	@mkdir -p .build
	$(CXX) -c -MP -MMD $(DFLAGS) $(CFLAGS) -o $@ $<

.build/debug:
	@mkdir -p .build
	@[ -n "$(DEBUG)" ] && { echo 'DEBUG ?= $(DEBUG)'; echo 'override OLDDEBUG = $(DEBUG)'; } >$@ || :
