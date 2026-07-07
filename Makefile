# Low-latency matching engine — build system.
# -O3 + -march=native matters here: we're chasing latency, so we want the
# compiler auto-vectorizing and tuning for THIS machine's CPU.

CXX      ?= clang++
CXXFLAGS := -std=c++20 -O3 -march=native -Wall -Wextra -Iinclude
LDFLAGS  :=

SRC      := src/order_book.cpp src/fast_order_book.cpp
DEMO     := src/main.cpp
TEST     := tests/test_order_book.cpp
BENCH    := bench/bench.cpp

BUILD    := build

.PHONY: all demo test bench clean run

all: demo test

$(BUILD):
	@mkdir -p $(BUILD)

# Demo binary — shows the engine working end to end.
demo: $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) $(DEMO) -o $(BUILD)/demo $(LDFLAGS)

run: demo
	@./$(BUILD)/demo

# Correctness tests (M2).
test: $(BUILD)
	@if [ -f $(TEST) ]; then \
		$(CXX) $(CXXFLAGS) $(SRC) $(TEST) -o $(BUILD)/test $(LDFLAGS) && ./$(BUILD)/test; \
	else echo "no tests yet (M2)"; fi

# Latency benchmark (M3).
bench: $(BUILD)
	@if [ -f $(BENCH) ]; then \
		$(CXX) $(CXXFLAGS) $(SRC) $(BENCH) -o $(BUILD)/bench $(LDFLAGS) && ./$(BUILD)/bench; \
	else echo "no benchmark yet (M3)"; fi

clean:
	rm -rf $(BUILD)
