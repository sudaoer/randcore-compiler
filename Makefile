CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin
SYSCONFDIR ?= /etc
SYSTEMD_UNIT_DIR ?= /etc/systemd/system
DOCDIR ?= $(PREFIX)/share/doc/randcore-compiler
PRIMARY := randcore-gcc
DAEMON := randcore-child-balancer
WRAPPERS ?= randcore-gcc randcore-g++ randcore-clang randcore-clang++ randcore-cc randcore-c++

.PHONY: all clean install

all: $(WRAPPERS) $(DAEMON)

$(PRIMARY): compiler-wrapper.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

$(DAEMON): child-balancer.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

$(filter-out $(PRIMARY),$(WRAPPERS)): $(PRIMARY)
	ln -sf $(PRIMARY) $@

randcore-%: $(PRIMARY)
	ln -sf $(PRIMARY) $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(SBINDIR)
	install -d $(DESTDIR)$(SYSCONFDIR)
	install -d $(DESTDIR)$(SYSTEMD_UNIT_DIR)
	install -d $(DESTDIR)$(DOCDIR)
	install -m 0755 $(PRIMARY) $(DESTDIR)$(BINDIR)
	install -m 0755 $(DAEMON) $(DESTDIR)$(SBINDIR)
	install -m 0644 systemd/randcore-child-balancer.service $(DESTDIR)$(SYSTEMD_UNIT_DIR)
	install -m 0644 examples/randcore-child-balancer.env $(DESTDIR)$(SYSCONFDIR)/randcore-child-balancer.env.example
	install -m 0644 README.md $(DESTDIR)$(DOCDIR)
	set -e; for wrapper in $(filter-out $(PRIMARY),$(WRAPPERS)); do \
		ln -sf $(PRIMARY) $(DESTDIR)$(BINDIR)/$$wrapper; \
	done

clean:
	rm -f $(PRIMARY) $(DAEMON) $(filter-out $(PRIMARY),$(WRAPPERS))
