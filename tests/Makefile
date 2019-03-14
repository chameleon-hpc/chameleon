CXX_FLAGS  		= -g -O3 -std=c++11 -qopenmp -Wall -Wno-unknown-pragmas
LINKER_FLAGS 	= -lchameleon -lstdc++
TEST_FILES 		= tests-example.cpp tests-annotations.cpp

compile: tests-main.o $(patsubst %.cpp, %.out, $(TEST_FILES))

%.out: %.cpp
	$(CXX) $(CXX_FLAGS) -o $@ $< tests-main.o $(LINKER_FLAGS)

tests-main.o:
	$(CXX) $(CXX_FLAGS) tests-main.cpp -c

run:
	for x in *.out; do ./$$x; done

clean:
	rm -rf *.out

clean-all:
	rm -rf *.out *.o