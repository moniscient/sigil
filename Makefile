CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -Isrc
SRCDIR = src
TESTDIR = tests
BUILDDIR = build

SRCS = $(SRCDIR)/main.c $(SRCDIR)/unicode.c $(SRCDIR)/errors.c \
       $(SRCDIR)/tokenizer.c $(SRCDIR)/parser.c $(SRCDIR)/algebra.c \
       $(SRCDIR)/desugarer.c $(SRCDIR)/types.c $(SRCDIR)/traits.c \
       $(SRCDIR)/resolver.c $(SRCDIR)/skw_emitter.c $(SRCDIR)/c_emitter.c \
       $(SRCDIR)/sigil_runtime.c
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

LIB_SRCS = $(filter-out $(SRCDIR)/main.c,$(SRCS))
LIB_OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(LIB_SRCS))

.PHONY: all clean test

all: $(BUILDDIR)/sigil

$(BUILDDIR)/sigil: $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Tests
$(BUILDDIR)/test_tokenizer: $(TESTDIR)/test_tokenizer.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_parser: $(TESTDIR)/test_parser.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_desugarer: $(TESTDIR)/test_desugarer.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_types: $(TESTDIR)/test_types.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_c_emitter: $(TESTDIR)/test_c_emitter.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILDDIR)/test_runtime: $(TESTDIR)/test_runtime.c $(BUILDDIR)/sigil_runtime.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(BUILDDIR)/sigil_runtime.o

test: $(BUILDDIR)/test_tokenizer $(BUILDDIR)/test_parser $(BUILDDIR)/test_desugarer $(BUILDDIR)/test_types $(BUILDDIR)/test_c_emitter $(BUILDDIR)/test_runtime $(BUILDDIR)/sigil
	$(BUILDDIR)/test_tokenizer
	$(BUILDDIR)/test_parser
	$(BUILDDIR)/test_desugarer
	$(BUILDDIR)/test_types
	$(BUILDDIR)/test_c_emitter
	$(BUILDDIR)/test_runtime
	bash $(TESTDIR)/test_e2e.sh

clean:
	rm -rf $(BUILDDIR)
