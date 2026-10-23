CXX ?= c++
PYTHON ?= python3
CPPFLAGS += -Iinclude -Ibuild/generated
CXXFLAGS ?= -O3 -DNDEBUG
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread

.PHONY: all test check clean native sanitize
all: build/wordle

build/generated/words.hpp: data/answers.txt data/guesses.txt scripts/embed_words.py
	@mkdir -p build/generated
	$(PYTHON) scripts/embed_words.py --answers data/answers.txt --guesses data/guesses.txt --output $@

build/solver.o: src/solver.cpp include/wordle/solver.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/generated/book.hpp: data/salet.easy.origwords.tree data/answers.txt data/guesses.txt scripts/embed_book.py scripts/embed_words.py
	@mkdir -p build/generated
	$(PYTHON) scripts/embed_book.py --output $@

build/book.o: src/book.cpp include/wordle/book.hpp build/generated/book.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/main.o: src/main.cpp include/wordle/solver.hpp include/wordle/book.hpp build/generated/words.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/wordle: build/solver.o build/book.o build/main.o
	$(CXX) $^ $(LDFLAGS) -o $@

build/solver_tests: tests/solver_tests.cpp build/solver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

build/book_tests: tests/book_tests.cpp build/book.o build/solver.o build/generated/words.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/book_tests.cpp build/book.o build/solver.o $(LDFLAGS) -o $@

test: build/solver_tests build/book_tests build/wordle
	./build/solver_tests
	./build/book_tests
	$(PYTHON) scripts/integration_test.py ./build/wordle

check: test

native:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -pthread -march=native' all

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='-O1 -g -std=c++20 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-pthread -fsanitize=address,undefined' test

clean:
	rm -rf build
