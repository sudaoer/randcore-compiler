CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
PRIMARY := randcore-gcc
WRAPPERS ?= randcore-gcc randcore-g++ randcore-clang randcore-clang++ randcore-cc randcore-c++

.PHONY: all clean install

all: $(WRAPPERS)

$(PRIMARY): compiler-wrapper.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

$(filter-out $(PRIMARY),$(WRAPPERS)): $(PRIMARY)
	cp -f $(PRIMARY) $@

randcore-%: $(PRIMARY)
	cp -f $(PRIMARY) $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(WRAPPERS) $(DESTDIR)$(BINDIR)

clean:
	rm -f $(PRIMARY) $(filter-out $(PRIMARY),$(WRAPPERS))
