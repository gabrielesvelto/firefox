/* import-globals-from crasher_subprocess_head.js */

// Let the event loop process a bit before crashing.
if (shouldDelay) {
  let count = 10;
  let shouldCrashNow = false;

  function delay() {
    if (count > 0) {
      count--;
      Services.tm.dispatchToMainThread({
        run: delay,
      });
    } else {
      shouldCrashNow = true;
    }
  }

  delay();
  Services.tm.spinEventLoopUntil(
    "Test(crasher_subprocess_tail.js:shouldDelay)",
    () => shouldCrashNow && !shouldWaitSetup
  );
}

// now actually crash
CrashTestUtils.crash(crashType);
