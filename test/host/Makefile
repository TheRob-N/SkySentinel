CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
LIB = ../../lib/sscore
SRCS = test_main.cpp $(wildcard $(LIB)/*.cpp)

run: test_bin
	./test_bin

test_bin: $(SRCS) $(wildcard $(LIB)/*.h)
	$(CXX) $(CXXFLAGS) -I$(LIB) $(SRCS) -o $@

clean:
	rm -f test_bin

.PHONY: run clean
