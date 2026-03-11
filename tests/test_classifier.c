#include <stdio.h>
#include <string.h>
#include "../src/sigil_classifier.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

static HardwareProfile make_hw(void) {
    HardwareProfile hw;
    hw.core_count = 8;
    hw.parallelism_threshold = 4;
    hw.gpu_width_threshold = 256;
    hw.thread_depth_threshold = 8;
    hw.thread_spawn_cost_ns = 50000;
    hw.gpu_available = true;
    return hw;
}

/* ── Test: terminated small graph → SEQ_OPTIMAL ──────────────────── */

static void test_terminated(void) {
    HardwareProfile hw = make_hw();
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = true;
    stats.depth = 3;
    stats.max_width = 4;
    stats.total_thunks = 10;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_SEQ_OPTIMAL, "terminated → SEQ_OPTIMAL");
}

/* ── Test: linear chain → SEQ_OBLIGATE ───────────────────────────── */

static void test_linear_chain(void) {
    HardwareProfile hw = make_hw();
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 20;
    stats.max_width = 1;
    stats.total_thunks = 20;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_SEQ_OBLIGATE, "linear chain → SEQ_OBLIGATE");
}

/* ── Test: narrow graph → SEQ_OPTIMAL ────────────────────────────── */

static void test_narrow(void) {
    HardwareProfile hw = make_hw();
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 10;
    stats.max_width = 3;  /* below threshold of 4 */
    stats.total_thunks = 30;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_SEQ_OPTIMAL, "narrow → SEQ_OPTIMAL");
}

/* ── Test: uniform wide → GPU ────────────────────────────────────── */

static void test_gpu(void) {
    HardwareProfile hw = make_hw();
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 5;
    stats.max_width = 512;
    stats.uniform_ops = true;
    stats.total_thunks = 1000;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_GPU, "uniform wide → GPU");
}

/* ── Test: GPU unavailable → falls through to THREAD or CORO ────── */

static void test_gpu_unavailable(void) {
    HardwareProfile hw = make_hw();
    hw.gpu_available = false;
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 10;
    stats.max_width = 512;
    stats.uniform_ops = true;
    stats.total_thunks = 1000;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_THREAD, "GPU unavailable + deep → THREAD");
}

/* ── Test: deep + wide → THREAD ──────────────────────────────────── */

static void test_thread(void) {
    HardwareProfile hw = make_hw();
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 12;
    stats.max_width = 50;
    stats.uniform_ops = false;
    stats.total_thunks = 500;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_THREAD, "deep+wide → THREAD");
}

/* ── Test: wide but shallow → CORO ───────────────────────────────── */

static void test_coro(void) {
    HardwareProfile hw = make_hw();
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 4;
    stats.max_width = 20;
    stats.uniform_ops = false;
    stats.total_thunks = 80;

    ExecutionBin bin = classify(&stats, &hw);
    ASSERT(bin == BIN_CORO, "wide+shallow → CORO");
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    test_terminated();
    test_linear_chain();
    test_narrow();
    test_gpu();
    test_gpu_unavailable();
    test_thread();
    test_coro();

    printf("test_classifier: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
