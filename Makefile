CXX = g++
CXXFLAGS = -Wall -Wextra -g

server: main.cpp EventLoop.cpp EventLoop.h Connection.cpp Connection.h http.cpp http.h
	$(CXX) $(CXXFLAGS) -o server main.cpp EventLoop.cpp Connection.cpp http.cpp


clean:
	rm -f server
