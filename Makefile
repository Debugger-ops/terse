# tersec — the Terse prompt compiler (C++17, no dependencies)
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PREFIX   ?= /usr/local

SRC = src/main.cpp src/util.cpp src/diag.cpp src/pp.cpp src/parse.cpp src/sema.cpp src/emit.cpp src/fmt.cpp src/fix.cpp
OBJ = $(SRC:src/%.cpp=build/%.o)

all: tersec terse

# `terse` is the same program; it also accepts `terse build|check|stats|fmt|fix`.
terse: tersec
	ln -sf tersec terse

tersec: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

build/%.o: src/%.cpp src/tersec.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: tersec
	@sh tests/run.sh

install: tersec
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 tersec $(DESTDIR)$(PREFIX)/bin/tersec
	ln -sf tersec $(DESTDIR)$(PREFIX)/bin/terse

clean:
	rm -rf build tersec terse

.PHONY: all test install clean
