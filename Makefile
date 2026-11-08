CXX ?= g++

darkrealm: main.cpp
	$(CXX) -O2 -o darkrealm main.cpp $(shell sdl2-config --cflags --libs) -lGLESv2 -static-libstdc++ -static-libgcc

clean:
	rm -f darkrealm
