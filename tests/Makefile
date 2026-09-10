# Host tests for the ESPN parser. Needs g++ and the ArduinoJson single header
# next to this file (ArduinoJson.h, version 7.4.3 to match ESPHome).
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -I../components/gameday -I.

all: build/test_parse
	./build/test_parse

build/test_parse: test_parse.cpp ../components/gameday/espn_parse.cpp ../components/gameday/espn_parse.h ../components/gameday/favorites.h ../components/gameday/espn_parse_impl.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) test_parse.cpp ../components/gameday/espn_parse.cpp -o $@

clean:
	rm -rf build
.PHONY: all clean
