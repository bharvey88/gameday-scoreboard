# Host tests for the ESPN parser. Needs g++ and the ArduinoJson single header
# next to this file (ArduinoJson.h, version 7.4.3 to match ESPHome).
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -I../components/gameday -I.

all: build/test_parse
	./build/test_parse

SRCS = test_parse.cpp golden_football.cpp ../components/gameday/espn_parse.cpp ../components/gameday/sport_baseball.cpp ../components/gameday/sport_hockey.cpp

build/test_parse: $(SRCS) $(wildcard ../components/gameday/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

clean:
	rm -rf build
.PHONY: all clean
