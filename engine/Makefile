# Standalone TMS57002 core (vendored MAME core + our shim), the board/synth engine, the oracle
# replay harness and the DSP ahead-of-time compiler.
# Native build (WSL/Linux):
#   make -C engine                                   -> engine/build/{replay,board_replay,make_slice}
#   make -C engine dspaot DATA=../out/win/data       -> generated/dspaot/dsp_aot_gen.inc (from your ROM data)
#   make -C engine fast                              -> engine/build/{dsp_fast_test,board_bench,synth_bench}
ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
TP   := $(ROOT)third_party/tms57002
SHIM := $(ROOT)mame_shim
B    := $(ROOT)build
GEN  := $(B)/gen/cpu/tms57002
REPO := $(abspath $(ROOT)..)
H8GEN := $(REPO)/generated/h8ctl/h8_gen.cpp
AOTGEN := $(REPO)/generated/dspaot/dsp_aot_gen.inc
DATA ?= $(REPO)/out/win/data
PY   ?= python3
CXX  ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -w
INC  := -I$(SHIM) -I$(TP) -I$(B)/gen -I$(ROOT)src -I$(REPO)/generated/dspaot

CORE_DEPS := $(GEN)/tms57002.hxx $(GEN)/tms57002_regops.hxx $(wildcard $(SHIM)/*.h) $(wildcard $(TP)/*.cpp) $(TP)/tms57002.h $(ROOT)src/dsp_core.cpp $(ROOT)src/dsp_fast.h

all: $(B)/replay $(B)/board_replay $(B)/make_slice

# The core is one translation unit (dsp_core.cpp includes the three vendored .cpp files) with the
# generated DSP programs when generated/dspaot/dsp_aot_gen.inc exists; dsp_core_none.o never has
# them (used by the generator itself).
$(B)/obj/dsp_core.o: $(CORE_DEPS) $(wildcard $(AOTGEN))
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $(ROOT)src/dsp_core.cpp -o $@

$(B)/obj/dsp_core_none.o: $(CORE_DEPS)
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -DKP_DSP_AOT_NONE -c $(ROOT)src/dsp_core.cpp -o $@

$(B)/libtms57002core.a: $(B)/obj/dsp_core.o
	ar rcs $@ $^

ENGINE_OBJ := $(B)/obj/prophecy_board.o $(B)/obj/performance.o
SYNTH_OBJ := $(B)/obj/prophecy_board.o $(B)/obj/synth.o $(B)/obj/h8ctl.o $(B)/obj/params.o $(B)/obj/perf.o $(B)/obj/sysex.o $(B)/obj/h8_gen.o

$(B)/obj/%.o: $(ROOT)src/%.cpp $(wildcard $(ROOT)src/*.h) $(wildcard $(ROOT)src/control/*.h) $(GEN)/tms57002.hxx $(wildcard $(SHIM)/*.h)
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(B)/obj/h8ctl.o: $(ROOT)src/control/h8ctl.cpp $(wildcard $(ROOT)src/control/*.h)
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(B)/obj/params.o: $(ROOT)src/control/params.cpp $(ROOT)src/control/params.h $(ROOT)src/control/params_table.inc
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(B)/obj/perf.o: $(ROOT)src/control/perf.cpp $(ROOT)src/control/perf.h $(ROOT)src/control/params.h
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(B)/obj/sysex.o: $(ROOT)src/control/sysex.cpp $(ROOT)src/control/sysex.h
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(B)/obj/h8_gen.o: $(H8GEN) $(ROOT)src/control/h8rt.h
	@mkdir -p $(B)/obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(B)/board_replay: $(ROOT)test/board_replay.cpp $(ENGINE_OBJ) $(B)/libtms57002core.a
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE_OBJ) $(B)/libtms57002core.a -o $@

$(B)/make_slice: $(ROOT)tools/make_slice.cpp $(ENGINE_OBJ) $(B)/libtms57002core.a
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE_OBJ) $(B)/libtms57002core.a -o $@

$(B)/replay: $(ROOT)test/replay.cpp $(B)/libtms57002core.a
	$(CXX) $(CXXFLAGS) $(INC) $< $(B)/libtms57002core.a -o $@

$(GEN)/tms57002.hxx: $(TP)/tmsinstr.lst $(TP)/tmsmake.py
	@mkdir -p $(GEN)
	$(PY) $(TP)/tmsmake.py s $(TP)/tmsinstr.lst $@

# Register-mode op bodies for the compiled programs (from the vendored core only).
$(GEN)/tms57002_regops.hxx: $(GEN)/tms57002.hxx $(REPO)/tools/tms_regops.py $(TP)/tms57002.cpp $(TP)/tms57002.h
	$(PY) $(REPO)/tools/tms_regops.py $(GEN)/tms57002.hxx $(TP)/tms57002.cpp $(TP)/tms57002.h $@

# H8 control code from your ic22 (make -C engine h8gen DATA=...): tools/h8dec.py -> IR -> tools/h8xlat.py
h8gen:
	@mkdir -p $(dir $(H8GEN))
	$(PY) $(REPO)/tools/h8_addrlist.py expand $(DATA)/ic22.bin $(REPO)/romdata/korgpro20_h8_seeds.txt -o $(dir $(H8GEN))h8_seeds.txt
	$(PY) $(REPO)/tools/h8_addrlist.py expand $(DATA)/ic22.bin $(REPO)/romdata/korgpro20_h8_entries.txt -o $(dir $(H8GEN))h8_entries.txt
	$(PY) $(REPO)/tools/h8dec.py $(DATA)/ic22.bin --entries $(dir $(H8GEN))h8_seeds.txt -o $(dir $(H8GEN))ic22.ir
	$(PY) $(REPO)/tools/h8xlat.py $(dir $(H8GEN))ic22.ir $(H8GEN) $(dir $(H8GEN))h8_entries.txt

# Native boot: state.bin + ram.bin from the ROMs (make -C engine boot DATA=... BOOT_OUT=...).
$(B)/native_boot: $(ROOT)tools/native_boot.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

# DSP ahead-of-time compiler and its gates.
$(B)/dsp_aot_gen: $(ROOT)tools/dsp_aot_gen.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core_none.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core_none.o -o $@

dspaot: $(B)/dsp_aot_gen
	@mkdir -p $(dir $(AOTGEN))
	$(B)/dsp_aot_gen $(DATA) $(AOTGEN)

$(B)/dsp_fast_test: $(ROOT)test/dsp_fast_test.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

$(B)/board_bench: $(ROOT)test/board_bench.cpp $(B)/obj/prophecy_board.o $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(B)/obj/prophecy_board.o $(B)/obj/dsp_core.o -o $@

$(B)/synth_bench: $(ROOT)test/synth_bench.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

$(B)/param_test: $(ROOT)test/param_test.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

$(B)/pe_test: $(ROOT)test/pe_test.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

$(B)/arp_test: $(ROOT)test/arp_test.cpp $(B)/obj/arp.o $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(B)/obj/arp.o $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

$(B)/program_test: $(ROOT)test/program_test.cpp $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

$(B)/poly_test: $(ROOT)test/poly_test.cpp $(B)/obj/poly.o $(SYNTH_OBJ) $(B)/obj/dsp_core.o
	$(CXX) $(CXXFLAGS) $(INC) $< $(B)/obj/poly.o $(SYNTH_OBJ) $(B)/obj/dsp_core.o -o $@

fast: $(B)/program_test $(B)/poly_test $(B)/dsp_fast_test $(B)/board_bench $(B)/synth_bench $(B)/param_test $(B)/pe_test $(B)/arp_test

clean:
	rm -rf $(B)

.PHONY: all clean dspaot fast h8gen
