# Offline harness. Not part of any device build.
#   make && ./shapones-host ~/Downloads/nesemu/tetris.nes 120 out.ppm
CORE  := ../../core
SRCS  := main.cpp host_intf.cpp $(wildcard $(CORE)/src/*.cpp)
shapones-host: $(SRCS) $(wildcard $(CORE)/include/shapones/*.hpp) $(wildcard $(CORE)/include/shapones/mappers/*.hpp)
	g++ -std=c++17 -O1 -g $(EXTRA) -I$(CORE)/include -o $@ $(SRCS)
clean:
	rm -f shapones-host *.ppm
