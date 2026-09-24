CXX = g++
CXXFLAGS = -Wall -Wextra -g

server: main.cpp EventLoop.cpp EventLoop.h Connection.cpp Connection.h
	$(CXX) $(CXXFLAGS) -o server main.cpp EventLoop.cpp Connection.cpp

clean:
	rm -f server
