CC      ?= gcc
CFLAGS  += -Wall -I..
LDFLAGS  = -static

.PHONY: all clean

all: pcie_edu_test

pcie_edu_test: pcie_edu_test.c ../pcie_edu.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	$(RM) pcie_edu_test
