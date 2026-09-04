// Test-only e2e hooks. The whole file compiles away without DLSSNR_TEST_HOOKS
// (build.sh --test defines it), so release builds ship none of this.
//
// Included by nr_runner.hpp inside namespace nr_runner, after State -- do not
// include directly. Each call site in the runner is an NR_TEST_HOOK_* macro
// that expands to nothing in release builds, so grepping NR_TEST_HOOK shows
// every place tests can take the wheel.
//
// Every hook is a lever, not logic: it must only call the same production
// entry points the UI calls, gated by an env var and announced with a
// TEST MODE log line. Levers:
//   DLSSNR_TEST_RETIRE_EVERY=N  force the overlay Apply button's retire every
//                               N evaluates (exercises retire/rebuild:
//                               graveyard, threading, feature recreate).

#if defined(DLSSNR_TEST_HOOKS)

inline void ApplyModelSettings();  // defined later in nr_runner.hpp

inline uint32_t test_retire_every = 0;

inline void TestHooksInit() {
  static bool done = false;
  if (done) return;
  done = true;
  char buf[16] = {};
  if (GetEnvironmentVariableA("DLSSNR_TEST_RETIRE_EVERY", buf, sizeof(buf)) > 0) {
    test_retire_every = (uint32_t)atoi(buf);
    if (test_retire_every != 0) {
      ngx_probe::Warnf(
          "nr-fwd: TEST MODE -- forcing a model-settings retire every %u evaluates "
          "(DLSSNR_TEST_RETIRE_EVERY)",
          test_retire_every);
    }
  }
}

inline void TestHookAfterEvaluate() {
  if (test_retire_every != 0 && s.feature != nullptr && s.eval_count != 0 &&
      s.eval_count % test_retire_every == 0) {
    ApplyModelSettings();
  }
}

#define NR_TEST_HOOKS_INIT() TestHooksInit()
#define NR_TEST_HOOK_AFTER_EVALUATE() TestHookAfterEvaluate()

#else

#define NR_TEST_HOOKS_INIT()
#define NR_TEST_HOOK_AFTER_EVALUATE()

#endif
