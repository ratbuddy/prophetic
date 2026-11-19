# Build the H8 analysis disassembler in WSL from the MAME checkout (BSD-3 h8d/h8hd sources).
MAME ?= $(HOME)/src/mame-profligacy
OUT  ?= $(HOME)/src/kp-tools
CXX  ?= g++
SRC  := $(MAME)/src/devices/cpu/h8
GEN  := $(MAME)/build/generated/emu

$(OUT)/h8dasm: h8dasm.cpp shim/emu.h $(SRC)/h8d.cpp $(SRC)/h8hd.cpp
	@mkdir -p $(OUT)
	$(CXX) -O2 -std=c++17 -w -Ishim -I$(SRC) -I$(MAME)/src/devices -I$(GEN) \
		h8dasm.cpp $(SRC)/h8d.cpp $(SRC)/h8hd.cpp -o $@
