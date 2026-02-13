# Asset Inventory Agent (C++17) - single binary: server/agent
# Build outputs: bin/asset_inventory(.exe)
# Notes:
# - On MinGW/MSYS2 we add -static-libstdc++ -static-libgcc to reduce DLL missing issues.
# - Uses native sockets only (no external dependencies).

CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
LDFLAGS ?=
SRC = src/main.cpp
BIN_DIR = bin

UNAME_S := $(shell uname -s 2>/dev/null)

# Windows/MSYS2 (uname may show MINGW64_NT-..., MSYS_NT-...)
ifneq (,$(findstring MINGW,$(UNAME_S)))
  EXE = .exe
  CXXFLAGS += -DWIN32_LEAN_AND_MEAN
  LDFLAGS += -lws2_32 -static-libstdc++ -static-libgcc
else ifneq (,$(findstring MSYS,$(UNAME_S)))
  EXE = .exe
  CXXFLAGS += -DWIN32_LEAN_AND_MEAN
  LDFLAGS += -lws2_32 -static-libstdc++ -static-libgcc
else
  EXE =
endif

all: $(BIN_DIR)/asset_inventory$(EXE)

$(BIN_DIR)/asset_inventory$(EXE): $(SRC)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	@rm -rf $(BIN_DIR)

.PHONY: all clean
