# CXX    := clang++-6.0
CXX      := g++
CFLAGS   := --std=gnu++2a -g -Wall -D_DEFAULT_SOURCE -fno-rtti -fno-exceptions \
            -fno-strict-aliasing -Wno-psabi
LFLAGS   := -Wall
LIBS     := -lpugixml
ALIBS    := -lfmt -lwebsockets -lcap -li2c

all: automation lutron relay
SRCS     := $(shell echo *.cpp)
AUTOMAT  := $(shell echo *.cpp | xargs -n1 | fgrep -v cmd | fgrep -v relaytest)
LUTRON   := $(shell echo *.cpp | xargs -n1 | fgrep -v main | grep -v relaytest)
RELAY    := $(shell echo *.cpp | xargs -n1 | fgrep -v cmd | grep -v main)

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
  LIBS   += $(ALIBS)
  ALIBS  :=
else
  DFLAGS := -O3
  CFLAGS += -DNDEBUG -ffunction-sections -fdata-sections -flto
  LFLAGS += -s -Xlinker --gc-sections
endif

.PHONY: clean
clean:
	rm -rf automation lutron relay .build
	@[ "$(DEBUG)" = 1 ] && { mkdir -p .build; { echo 'DEBUG ?= 1'; echo 'override OLDDEBUG := 1'; } >.build/debug; } || :

automation: $(patsubst %.cpp,.build/%.o,$(AUTOMAT)) .build/debug
	$(CXX) $(DFLAGS) $(LFLAGS) -o $@ $(patsubst %.cpp,.build/%.o,$(AUTOMAT)) $(LIBS) $(ALIBS)

lutron: $(patsubst %.cpp,.build/%.o,$(LUTRON)) .build/debug
	$(CXX) $(DFLAGS) $(LFLAGS) -o $@ $(patsubst %.cpp,.build/%.o,$(LUTRON)) $(LIBS)

relay: $(patsubst %.cpp,.build/%.o,$(RELAY)) .build/debug
	$(CXX) $(DFLAGS) $(LFLAGS) -o $@ $(patsubst %.cpp,.build/%.o,$(RELAY)) $(ALIBS)

.build/%.o: %.cpp | .build/debug
	@mkdir -p .build
	$(CXX) -c -MP -MMD $(DFLAGS) $(CFLAGS) -o $@ $<

.build/debug:
	@mkdir -p .build
	@[ -n "$(DEBUG)" ] && { echo 'DEBUG ?= $(DEBUG)'; echo 'override OLDDEBUG = $(DEBUG)'; } >$@ || :
