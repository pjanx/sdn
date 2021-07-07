.POSIX:
SHELL = /bin/sh
CXXFLAGS = -g -std=c++14 -Wall -Wextra -pedantic
CPPFLAGS = `sed -ne '/^project (\([^ )]*\) VERSION \([^ )]*\).*/ \
	s//-DPROJECT_NAME="\1" -DPROJECT_VERSION="\2"/p' CMakeLists.txt`

sdn: sdn.cpp CMakeLists.txt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< -o sdn \
	-lacl `pkg-config --libs --cflags ncursesw`
static: sdn.cpp CMakeLists.txt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< -o sdn \
	-static-libstdc++ \
	-Wl,--start-group,-Bstatic \
	-lacl `pkg-config --static --libs --cflags ncursesw` \
	-Wl,--end-group,-Bdynamic
clean:
	rm -f sdn

.PHONY: static clean
