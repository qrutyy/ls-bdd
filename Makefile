# Builds the driver from the root of the repo: every target is passed on to
# src/Makefile, along with the variables given on the command line
# (make TY=sy, make init DS=ht, ...).

SRC := src
TY ?= lf

.DEFAULT_GOAL := all

all: modules

# Kbuild needs the type of data structures, src/Makefile only passes it on.
modules:
	$(MAKE) -C $(SRC) type=$(TY) modules

# src/Makefile cleans test/ as well.
clean:
	$(MAKE) -C $(SRC) clean

# The KUnit suites built into the module: needs root and a built module.
test:
	$(MAKE) -C $(SRC) kunit

# Keeps make from trying to remake this file through the rule below.
Makefile: ;

%:
	$(MAKE) -C $(SRC) $@

.PHONY: all modules clean test Makefile
