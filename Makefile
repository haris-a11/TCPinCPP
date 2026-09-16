CXX = g++
CXXFLAGS = -Wall -Wextra -g

server: main.cpp EventLoop.cpp EventLoop.h
	$(CXX) $(CXXFLAGS) -o server main.cpp EventLoop.cpp

clean:
	rm -f server
