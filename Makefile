CXX ?= c++
PYTHON ?= python3
CPPFLAGS += -Isrc -Ibuild/generated
CXXFLAGS ?= -O3 -DNDEBUG
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread

.PHONY: all test clean native sanitize
all: build/wordle

build/generated/words.hpp: data/answers.txt data/guesses.txt scripts/embed_words.py
	@mkdir -p build/generated
	$(PYTHON) scripts/embed_words.py --answers data/answers.txt --guesses data/guesses.txt --output $@

build/solver.o: src/solver.cpp src/solver.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/generated/book_data.hpp: data/salet.easy.origwords.tree data/answers.txt data/guesses.txt scripts/embed_book.py scripts/embed_words.py
	@mkdir -p build/generated
	$(PYTHON) scripts/embed_book.py --output $@

build/book.o: src/book.cpp src/book.hpp src/solver.hpp build/generated/book_data.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/main.o: src/main.cpp src/solver.hpp src/book.hpp build/generated/words.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/wordle: build/solver.o build/book.o build/main.o
	$(CXX) $^ $(LDFLAGS) -o $@

build/solver_tests: tests/solver_tests.cpp src/solver.hpp build/solver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/solver_tests.cpp build/solver.o $(LDFLAGS) -o $@

build/book_tests: tests/book_tests.cpp src/book.hpp src/solver.hpp build/book.o build/solver.o build/generated/words.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/book_tests.cpp build/book.o build/solver.o $(LDFLAGS) -o $@

test: build/solver_tests build/book_tests build/wordle
	./build/solver_tests
	./build/book_tests
	$(PYTHON) tests/test_cli.py ./build/wordle

native:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -pthread -march=native' all

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='-O1 -g -std=c++20 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-pthread -fsanitize=address,undefined' test

clean:
	rm -rf build
