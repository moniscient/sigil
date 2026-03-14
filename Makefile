CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -Isrc
LDFLAGS = -pthread
SRCDIR = src
TESTDIR = tests
BUILDDIR = build

SRCS = $(SRCDIR)/main.c $(SRCDIR)/unicode.c $(SRCDIR)/errors.c \
       $(SRCDIR)/tokenizer.c $(SRCDIR)/parser.c $(SRCDIR)/algebra.c \
       $(SRCDIR)/desugarer.c $(SRCDIR)/types.c $(SRCDIR)/traits.c \
       $(SRCDIR)/resolver.c $(SRCDIR)/skw_emitter.c $(SRCDIR)/c_emitter.c \
       $(SRCDIR)/parallel.c $(SRCDIR)/sigil_runtime.c \
       $(SRCDIR)/sigil_thunk.c $(SRCDIR)/sigil_expander.c \
       $(SRCDIR)/sigil_classifier.c $(SRCDIR)/sigil_hardware.c \
       $(SRCDIR)/sigil_exec_seq.c $(SRCDIR)/sigil_exec_coro.c \
       $(SRCDIR)/sigil_exec_thread.c $(SRCDIR)/sigil_exec_gpu.c \
       $(SRCDIR)/sigil_executor.c
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

LIB_SRCS = $(filter-out $(SRCDIR)/main.c,$(SRCS))
LIB_OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(LIB_SRCS))

# Thunk runtime objects (for compiled Sigil programs)
THUNK_RT_SRCS = $(SRCDIR)/sigil_runtime.c $(SRCDIR)/sigil_thunk.c \
                $(SRCDIR)/sigil_expander.c $(SRCDIR)/sigil_classifier.c \
                $(SRCDIR)/sigil_hardware.c $(SRCDIR)/sigil_exec_seq.c \
                $(SRCDIR)/sigil_exec_coro.c $(SRCDIR)/sigil_exec_thread.c \
                $(SRCDIR)/sigil_exec_gpu.c $(SRCDIR)/sigil_executor.c
THUNK_RT_OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(THUNK_RT_SRCS))

.PHONY: all clean test

all: $(BUILDDIR)/sigil

$(BUILDDIR)/sigil: $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Tests
$(BUILDDIR)/test_tokenizer: $(TESTDIR)/test_tokenizer.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_parser: $(TESTDIR)/test_parser.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_desugarer: $(TESTDIR)/test_desugarer.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_types: $(TESTDIR)/test_types.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_c_emitter: $(TESTDIR)/test_c_emitter.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_runtime: $(TESTDIR)/test_runtime.c $(BUILDDIR)/sigil_runtime.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(BUILDDIR)/sigil_runtime.o

$(BUILDDIR)/test_thunk: $(TESTDIR)/test_thunk.c $(BUILDDIR)/sigil_thunk.o $(BUILDDIR)/sigil_runtime.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(BUILDDIR)/sigil_thunk.o $(BUILDDIR)/sigil_runtime.o

$(BUILDDIR)/test_classifier: $(TESTDIR)/test_classifier.c $(BUILDDIR)/sigil_classifier.o $(BUILDDIR)/sigil_expander.o $(BUILDDIR)/sigil_thunk.o $(BUILDDIR)/sigil_runtime.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(BUILDDIR)/sigil_classifier.o $(BUILDDIR)/sigil_expander.o $(BUILDDIR)/sigil_thunk.o $(BUILDDIR)/sigil_runtime.o

$(BUILDDIR)/test_validation: $(TESTDIR)/test_validation.c $(THUNK_RT_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(THUNK_RT_OBJS)

test_validation: $(BUILDDIR)/test_validation
	$(BUILDDIR)/test_validation

test: $(BUILDDIR)/test_tokenizer $(BUILDDIR)/test_parser $(BUILDDIR)/test_desugarer $(BUILDDIR)/test_types $(BUILDDIR)/test_c_emitter $(BUILDDIR)/test_runtime $(BUILDDIR)/test_thunk $(BUILDDIR)/test_classifier $(BUILDDIR)/sigil
	$(BUILDDIR)/test_tokenizer
	$(BUILDDIR)/test_parser
	$(BUILDDIR)/test_desugarer
	$(BUILDDIR)/test_types
	$(BUILDDIR)/test_c_emitter
	$(BUILDDIR)/test_runtime
	$(BUILDDIR)/test_thunk
	$(BUILDDIR)/test_classifier
	bash $(TESTDIR)/test_e2e.sh

clean:
	rm -rf $(BUILDDIR)
