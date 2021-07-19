.POSIX:
SHELL = /bin/sh
CXXFLAGS = -g -std=c++14 -Wall -Wextra -Wno-misleading-indentation -pedantic
CPPFLAGS = `sed -ne '/^project (\([^ )]*\) VERSION \([^ )]*\).*/ \
	s//-DPROJECT_NAME="\1" -DPROJECT_VERSION="\2"/p' CMakeLists.txt`

sdn: sdn.cpp CMakeLists.txt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< -o $@ \
	-lacl `pkg-config --libs --cflags ncursesw`
sdn-static: sdn.cpp CMakeLists.txt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< -o $@ \
	-static-libstdc++ \
	-Wl,--start-group,-Bstatic \
	-lacl `pkg-config --static --libs --cflags ncursesw` \
	-Wl,--end-group,-Bdynamic
clean:
	rm -f sdn sdn-static

.PHONY: clean
