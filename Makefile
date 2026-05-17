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
	ln -sf $(PRIMARY) $@

randcore-%: $(PRIMARY)
	ln -sf $(PRIMARY) $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(PRIMARY) $(DESTDIR)$(BINDIR)
	set -e; for wrapper in $(filter-out $(PRIMARY),$(WRAPPERS)); do \
		ln -sf $(PRIMARY) $(DESTDIR)$(BINDIR)/$$wrapper; \
	done

clean:
	rm -f $(PRIMARY) $(filter-out $(PRIMARY),$(WRAPPERS))
