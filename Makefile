PYTHON ?= python3
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all build test install uninstall
all: build

build:
	$(PYTHON) tools/build.py

test: build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc tests/fish-motion-test.c src/fish-motion.c -o build/fish-motion-test $(LDFLAGS) -lm
	./build/fish-motion-test
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

install:
	$(PYTHON) tools/manage.py install

uninstall:
	$(PYTHON) tools/manage.py uninstall
