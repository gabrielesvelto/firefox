/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BaseProfiler.h"

#include "mozilla/Attributes.h"
#include "mozilla/BaseAndGeckoProfilerDetail.h"
#include "mozilla/BaseProfileJSONWriter.h"
#include "mozilla/BaseProfilerDetail.h"
#include "mozilla/FailureLatch.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/NotNull.h"
#include "mozilla/ProgressLogger.h"
#include "mozilla/ProportionValue.h"

#ifdef MOZ_GECKO_PROFILER
#  include "mozilla/BaseProfilerMarkerTypes.h"
#  include "mozilla/leb128iterator.h"
#  include "mozilla/ModuloBuffer.h"
#  include "mozilla/mozalloc.h"
#  include "mozilla/PowerOfTwo.h"
#  include "mozilla/ProfileBufferChunk.h"
#  include "mozilla/ProfileBufferChunkManagerSingle.h"
#  include "mozilla/ProfileBufferChunkManagerWithLocalLimit.h"
#  include "mozilla/ProfileBufferControlledChunkManager.h"
#  include "mozilla/ProfileChunkedBuffer.h"
#  include "mozilla/Vector.h"
#endif  // MOZ_GECKO_PROFILER

#if defined(_MSC_VER) || defined(__MINGW32__)
#  include <windows.h>
#  include <mmsystem.h>
#  include <process.h>
#else
#  include <errno.h>
#  include <time.h>
#endif

#include <algorithm>
#include <atomic>
#include <iostream>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>

void TestFailureLatch() {
  printf("TestFailureLatch...\n");

  // Test infallible latch.
  {
    mozilla::FailureLatchInfallibleSource& infallibleLatch =
        mozilla::FailureLatchInfallibleSource::Singleton();

    MOZ_RELEASE_ASSERT(!infallibleLatch.Fallible());
    MOZ_RELEASE_ASSERT(!infallibleLatch.Failed());
    MOZ_RELEASE_ASSERT(!infallibleLatch.GetFailure());
    MOZ_RELEASE_ASSERT(&infallibleLatch.SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
    MOZ_RELEASE_ASSERT(&std::as_const(infallibleLatch).SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
  }

  // Test failure latch basic functions.
  {
    mozilla::FailureLatchSource failureLatch;

    MOZ_RELEASE_ASSERT(failureLatch.Fallible());
    MOZ_RELEASE_ASSERT(!failureLatch.Failed());
    MOZ_RELEASE_ASSERT(!failureLatch.GetFailure());
    MOZ_RELEASE_ASSERT(&failureLatch.SourceFailureLatch() == &failureLatch);
    MOZ_RELEASE_ASSERT(&std::as_const(failureLatch).SourceFailureLatch() ==
                       &failureLatch);

    failureLatch.SetFailure("error");

    MOZ_RELEASE_ASSERT(failureLatch.Fallible());
    MOZ_RELEASE_ASSERT(failureLatch.Failed());
    MOZ_RELEASE_ASSERT(failureLatch.GetFailure());
    MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "error") == 0);

    failureLatch.SetFailure("later error");

    MOZ_RELEASE_ASSERT(failureLatch.Fallible());
    MOZ_RELEASE_ASSERT(failureLatch.Failed());
    MOZ_RELEASE_ASSERT(failureLatch.GetFailure());
    MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "error") == 0);
  }

  // Test SetFailureFrom.
  {
    mozilla::FailureLatchSource failureLatch;

    MOZ_RELEASE_ASSERT(!failureLatch.Failed());
    failureLatch.SetFailureFrom(failureLatch);
    MOZ_RELEASE_ASSERT(!failureLatch.Failed());
    MOZ_RELEASE_ASSERT(!failureLatch.GetFailure());

    // SetFailureFrom with no error.
    {
      mozilla::FailureLatchSource failureLatchInnerOk;
      MOZ_RELEASE_ASSERT(!failureLatchInnerOk.Failed());
      MOZ_RELEASE_ASSERT(!failureLatchInnerOk.GetFailure());

      MOZ_RELEASE_ASSERT(!failureLatch.Failed());
      failureLatch.SetFailureFrom(failureLatchInnerOk);
      MOZ_RELEASE_ASSERT(!failureLatch.Failed());

      MOZ_RELEASE_ASSERT(!failureLatchInnerOk.Failed());
      MOZ_RELEASE_ASSERT(!failureLatchInnerOk.GetFailure());
    }
    MOZ_RELEASE_ASSERT(!failureLatch.Failed());
    MOZ_RELEASE_ASSERT(!failureLatch.GetFailure());

    // SetFailureFrom with error.
    {
      mozilla::FailureLatchSource failureLatchInnerError;
      MOZ_RELEASE_ASSERT(!failureLatchInnerError.Failed());
      MOZ_RELEASE_ASSERT(!failureLatchInnerError.GetFailure());

      failureLatchInnerError.SetFailure("inner error");
      MOZ_RELEASE_ASSERT(failureLatchInnerError.Failed());
      MOZ_RELEASE_ASSERT(
          strcmp(failureLatchInnerError.GetFailure(), "inner error") == 0);

      MOZ_RELEASE_ASSERT(!failureLatch.Failed());
      failureLatch.SetFailureFrom(failureLatchInnerError);
      MOZ_RELEASE_ASSERT(failureLatch.Failed());

      MOZ_RELEASE_ASSERT(failureLatchInnerError.Failed());
      MOZ_RELEASE_ASSERT(
          strcmp(failureLatchInnerError.GetFailure(), "inner error") == 0);
    }
    MOZ_RELEASE_ASSERT(failureLatch.Failed());
    MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "inner error") == 0);

    failureLatch.SetFailureFrom(failureLatch);
    MOZ_RELEASE_ASSERT(failureLatch.Failed());
    MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "inner error") == 0);

    // SetFailureFrom with error again, ignored.
    {
      mozilla::FailureLatchSource failureLatchInnerError;
      failureLatchInnerError.SetFailure("later inner error");
      MOZ_RELEASE_ASSERT(failureLatchInnerError.Failed());
      MOZ_RELEASE_ASSERT(strcmp(failureLatchInnerError.GetFailure(),
                                "later inner error") == 0);

      MOZ_RELEASE_ASSERT(failureLatch.Failed());
      failureLatch.SetFailureFrom(failureLatchInnerError);
      MOZ_RELEASE_ASSERT(failureLatch.Failed());

      MOZ_RELEASE_ASSERT(failureLatchInnerError.Failed());
      MOZ_RELEASE_ASSERT(strcmp(failureLatchInnerError.GetFailure(),
                                "later inner error") == 0);
    }
    MOZ_RELEASE_ASSERT(failureLatch.Failed());
    MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "inner error") == 0);
  }

  // Test FAILURELATCH_IMPL_PROXY
  {
    class Proxy final : public mozilla::FailureLatch {
     public:
      explicit Proxy(mozilla::FailureLatch& aFailureLatch)
          : mFailureLatch(WrapNotNull(&aFailureLatch)) {}

      void Set(mozilla::FailureLatch& aFailureLatch) {
        mFailureLatch = WrapNotNull(&aFailureLatch);
      }

      FAILURELATCH_IMPL_PROXY(*mFailureLatch)

     private:
      mozilla::NotNull<mozilla::FailureLatch*> mFailureLatch;
    };

    Proxy proxy{mozilla::FailureLatchInfallibleSource::Singleton()};

    MOZ_RELEASE_ASSERT(!proxy.Fallible());
    MOZ_RELEASE_ASSERT(!proxy.Failed());
    MOZ_RELEASE_ASSERT(!proxy.GetFailure());
    MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
    MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());

    // Error from proxy.
    {
      mozilla::FailureLatchSource failureLatch;
      proxy.Set(failureLatch);
      MOZ_RELEASE_ASSERT(proxy.Fallible());
      MOZ_RELEASE_ASSERT(!proxy.Failed());
      MOZ_RELEASE_ASSERT(!proxy.GetFailure());
      MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() == &failureLatch);
      MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                         &failureLatch);

      proxy.SetFailure("error");
      MOZ_RELEASE_ASSERT(proxy.Failed());
      MOZ_RELEASE_ASSERT(strcmp(proxy.GetFailure(), "error") == 0);
      MOZ_RELEASE_ASSERT(failureLatch.Failed());
      MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "error") == 0);

      // Don't forget to stop pointing at soon-to-be-destroyed object.
      proxy.Set(mozilla::FailureLatchInfallibleSource::Singleton());
    }

    // Error from proxy's origin.
    {
      mozilla::FailureLatchSource failureLatch;
      proxy.Set(failureLatch);
      MOZ_RELEASE_ASSERT(proxy.Fallible());
      MOZ_RELEASE_ASSERT(!proxy.Failed());
      MOZ_RELEASE_ASSERT(!proxy.GetFailure());
      MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() == &failureLatch);
      MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                         &failureLatch);

      failureLatch.SetFailure("error");
      MOZ_RELEASE_ASSERT(proxy.Failed());
      MOZ_RELEASE_ASSERT(strcmp(proxy.GetFailure(), "error") == 0);
      MOZ_RELEASE_ASSERT(failureLatch.Failed());
      MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "error") == 0);

      // Don't forget to stop pointing at soon-to-be-destroyed object.
      proxy.Set(mozilla::FailureLatchInfallibleSource::Singleton());
    }

    MOZ_RELEASE_ASSERT(!proxy.Fallible());
    MOZ_RELEASE_ASSERT(!proxy.Failed());
    MOZ_RELEASE_ASSERT(!proxy.GetFailure());
    MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
    MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
  }

  // Test FAILURELATCH_IMPL_PROXY_OR_INFALLIBLE
  {
    class ProxyOrNull final : public mozilla::FailureLatch {
     public:
      ProxyOrNull() = default;

      void Set(mozilla::FailureLatch* aFailureLatchOrNull) {
        mFailureLatchOrNull = aFailureLatchOrNull;
      }

      FAILURELATCH_IMPL_PROXY_OR_INFALLIBLE(mFailureLatchOrNull, ProxyOrNull)

     private:
      mozilla::FailureLatch* mFailureLatchOrNull = nullptr;
    };

    ProxyOrNull proxy;

    MOZ_RELEASE_ASSERT(!proxy.Fallible());
    MOZ_RELEASE_ASSERT(!proxy.Failed());
    MOZ_RELEASE_ASSERT(!proxy.GetFailure());
    MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
    MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());

    // Error from proxy.
    {
      mozilla::FailureLatchSource failureLatch;
      proxy.Set(&failureLatch);
      MOZ_RELEASE_ASSERT(proxy.Fallible());
      MOZ_RELEASE_ASSERT(!proxy.Failed());
      MOZ_RELEASE_ASSERT(!proxy.GetFailure());
      MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() == &failureLatch);
      MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                         &failureLatch);

      proxy.SetFailure("error");
      MOZ_RELEASE_ASSERT(proxy.Failed());
      MOZ_RELEASE_ASSERT(strcmp(proxy.GetFailure(), "error") == 0);
      MOZ_RELEASE_ASSERT(failureLatch.Failed());
      MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "error") == 0);

      // Don't forget to stop pointing at soon-to-be-destroyed object.
      proxy.Set(nullptr);
    }

    // Error from proxy's origin.
    {
      mozilla::FailureLatchSource failureLatch;
      proxy.Set(&failureLatch);
      MOZ_RELEASE_ASSERT(proxy.Fallible());
      MOZ_RELEASE_ASSERT(!proxy.Failed());
      MOZ_RELEASE_ASSERT(!proxy.GetFailure());
      MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() == &failureLatch);
      MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                         &failureLatch);

      failureLatch.SetFailure("error");
      MOZ_RELEASE_ASSERT(proxy.Failed());
      MOZ_RELEASE_ASSERT(strcmp(proxy.GetFailure(), "error") == 0);
      MOZ_RELEASE_ASSERT(failureLatch.Failed());
      MOZ_RELEASE_ASSERT(strcmp(failureLatch.GetFailure(), "error") == 0);

      // Don't forget to stop pointing at soon-to-be-destroyed object.
      proxy.Set(nullptr);
    }

    MOZ_RELEASE_ASSERT(!proxy.Fallible());
    MOZ_RELEASE_ASSERT(!proxy.Failed());
    MOZ_RELEASE_ASSERT(!proxy.GetFailure());
    MOZ_RELEASE_ASSERT(&proxy.SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
    MOZ_RELEASE_ASSERT(&std::as_const(proxy).SourceFailureLatch() ==
                       &mozilla::FailureLatchInfallibleSource::Singleton());
  }

  printf("TestFailureLatch done\n");
}

void TestProfilerUtils() {
  printf("TestProfilerUtils...\n");

  {
    using mozilla::baseprofiler::BaseProfilerProcessId;
    using Number = BaseProfilerProcessId::NumberType;
    static constexpr Number scMaxNumber = std::numeric_limits<Number>::max();

    static_assert(
        BaseProfilerProcessId{}.ToNumber() == 0,
        "These tests assume that the unspecified process id number is 0; "
        "if this fails, please update these tests accordingly");

    static_assert(!BaseProfilerProcessId{}.IsSpecified());
    static_assert(!BaseProfilerProcessId::FromNumber(0).IsSpecified());
    static_assert(BaseProfilerProcessId::FromNumber(1).IsSpecified());
    static_assert(BaseProfilerProcessId::FromNumber(123).IsSpecified());
    static_assert(BaseProfilerProcessId::FromNumber(scMaxNumber).IsSpecified());

    static_assert(BaseProfilerProcessId::FromNumber(Number(1)).ToNumber() ==
                  Number(1));
    static_assert(BaseProfilerProcessId::FromNumber(Number(123)).ToNumber() ==
                  Number(123));
    static_assert(BaseProfilerProcessId::FromNumber(scMaxNumber).ToNumber() ==
                  scMaxNumber);

    static_assert(BaseProfilerProcessId{} == BaseProfilerProcessId{});
    static_assert(BaseProfilerProcessId::FromNumber(Number(123)) ==
                  BaseProfilerProcessId::FromNumber(Number(123)));
    static_assert(BaseProfilerProcessId{} !=
                  BaseProfilerProcessId::FromNumber(Number(123)));
    static_assert(BaseProfilerProcessId::FromNumber(Number(123)) !=
                  BaseProfilerProcessId{});
    static_assert(BaseProfilerProcessId::FromNumber(Number(123)) !=
                  BaseProfilerProcessId::FromNumber(scMaxNumber));
    static_assert(BaseProfilerProcessId::FromNumber(scMaxNumber) !=
                  BaseProfilerProcessId::FromNumber(Number(123)));

    // Verify trivial-copyability by memcpy'ing to&from same-size storage.
    static_assert(std::is_trivially_copyable_v<BaseProfilerProcessId>);
    BaseProfilerProcessId pid;
    MOZ_RELEASE_ASSERT(!pid.IsSpecified());
    Number pidStorage;
    static_assert(sizeof(pidStorage) == sizeof(pid));
    // Copy from BaseProfilerProcessId to storage. Note: We cannot assume that
    // this is equal to what ToNumber() gives us. All we can do is verify that
    // copying from storage back to BaseProfilerProcessId works as expected.
    std::memcpy(&pidStorage, &pid, sizeof(pidStorage));
    BaseProfilerProcessId pid2 = BaseProfilerProcessId::FromNumber(2);
    MOZ_RELEASE_ASSERT(pid2.IsSpecified());
    std::memcpy(&pid2, &pidStorage, sizeof(pid));
    MOZ_RELEASE_ASSERT(!pid2.IsSpecified());

    pid = BaseProfilerProcessId::FromNumber(123);
    std::memcpy(&pidStorage, &pid, sizeof(pidStorage));
    pid2 = BaseProfilerProcessId{};
    MOZ_RELEASE_ASSERT(!pid2.IsSpecified());
    std::memcpy(&pid2, &pidStorage, sizeof(pid));
    MOZ_RELEASE_ASSERT(pid2.IsSpecified());
    MOZ_RELEASE_ASSERT(pid2.ToNumber() == 123);

    // No conversions to/from numbers.
    static_assert(!std::is_constructible_v<BaseProfilerProcessId, Number>);
    static_assert(!std::is_assignable_v<BaseProfilerProcessId, Number>);
    static_assert(!std::is_constructible_v<Number, BaseProfilerProcessId>);
    static_assert(!std::is_assignable_v<Number, BaseProfilerProcessId>);

    static_assert(
        std::is_same_v<
            decltype(mozilla::baseprofiler::profiler_current_process_id()),
            BaseProfilerProcessId>);
    MOZ_RELEASE_ASSERT(
        mozilla::baseprofiler::profiler_current_process_id().IsSpecified());
  }

  {
    mozilla::baseprofiler::profiler_init_main_thread_id();

    using mozilla::baseprofiler::BaseProfilerThreadId;
    using Number = BaseProfilerThreadId::NumberType;
    static constexpr Number scMaxNumber = std::numeric_limits<Number>::max();

    static_assert(
        BaseProfilerThreadId{}.ToNumber() == 0,
        "These tests assume that the unspecified thread id number is 0; "
        "if this fails, please update these tests accordingly");

    static_assert(!BaseProfilerThreadId{}.IsSpecified());
    static_assert(!BaseProfilerThreadId::FromNumber(0).IsSpecified());
    static_assert(BaseProfilerThreadId::FromNumber(1).IsSpecified());
    static_assert(BaseProfilerThreadId::FromNumber(123).IsSpecified());
    static_assert(BaseProfilerThreadId::FromNumber(scMaxNumber).IsSpecified());

    static_assert(BaseProfilerThreadId::FromNumber(Number(1)).ToNumber() ==
                  Number(1));
    static_assert(BaseProfilerThreadId::FromNumber(Number(123)).ToNumber() ==
                  Number(123));
    static_assert(BaseProfilerThreadId::FromNumber(scMaxNumber).ToNumber() ==
                  scMaxNumber);

    static_assert(BaseProfilerThreadId{} == BaseProfilerThreadId{});
    static_assert(BaseProfilerThreadId::FromNumber(Number(123)) ==
                  BaseProfilerThreadId::FromNumber(Number(123)));
    static_assert(BaseProfilerThreadId{} !=
                  BaseProfilerThreadId::FromNumber(Number(123)));
    static_assert(BaseProfilerThreadId::FromNumber(Number(123)) !=
                  BaseProfilerThreadId{});
    static_assert(BaseProfilerThreadId::FromNumber(Number(123)) !=
                  BaseProfilerThreadId::FromNumber(scMaxNumber));
    static_assert(BaseProfilerThreadId::FromNumber(scMaxNumber) !=
                  BaseProfilerThreadId::FromNumber(Number(123)));

    // Verify trivial-copyability by memcpy'ing to&from same-size storage.
    static_assert(std::is_trivially_copyable_v<BaseProfilerThreadId>);
    BaseProfilerThreadId tid;
    MOZ_RELEASE_ASSERT(!tid.IsSpecified());
    Number tidStorage;
    static_assert(sizeof(tidStorage) == sizeof(tid));
    // Copy from BaseProfilerThreadId to storage. Note: We cannot assume that
    // this is equal to what ToNumber() gives us. All we can do is verify that
    // copying from storage back to BaseProfilerThreadId works as expected.
    std::memcpy(&tidStorage, &tid, sizeof(tidStorage));
    BaseProfilerThreadId tid2 = BaseProfilerThreadId::FromNumber(2);
    MOZ_RELEASE_ASSERT(tid2.IsSpecified());
    std::memcpy(&tid2, &tidStorage, sizeof(tid));
    MOZ_RELEASE_ASSERT(!tid2.IsSpecified());

    tid = BaseProfilerThreadId::FromNumber(Number(123));
    std::memcpy(&tidStorage, &tid, sizeof(tidStorage));
    tid2 = BaseProfilerThreadId{};
    MOZ_RELEASE_ASSERT(!tid2.IsSpecified());
    std::memcpy(&tid2, &tidStorage, sizeof(tid));
    MOZ_RELEASE_ASSERT(tid2.IsSpecified());
    MOZ_RELEASE_ASSERT(tid2.ToNumber() == Number(123));

    // No conversions to/from numbers.
    static_assert(!std::is_constructible_v<BaseProfilerThreadId, Number>);
    static_assert(!std::is_assignable_v<BaseProfilerThreadId, Number>);
    static_assert(!std::is_constructible_v<Number, BaseProfilerThreadId>);
    static_assert(!std::is_assignable_v<Number, BaseProfilerThreadId>);

    static_assert(std::is_same_v<
                  decltype(mozilla::baseprofiler::profiler_current_thread_id()),
                  BaseProfilerThreadId>);
    BaseProfilerThreadId mainTestThreadId =
        mozilla::baseprofiler::profiler_current_thread_id();
    MOZ_RELEASE_ASSERT(mainTestThreadId.IsSpecified());

    BaseProfilerThreadId mainThreadId =
        mozilla::baseprofiler::profiler_main_thread_id();
    MOZ_RELEASE_ASSERT(mainThreadId.IsSpecified());

    MOZ_RELEASE_ASSERT(mainThreadId == mainTestThreadId,
                       "Test should run on the main thread");
    MOZ_RELEASE_ASSERT(mozilla::baseprofiler::profiler_is_main_thread());

    std::thread testThread([&]() {
      const BaseProfilerThreadId testThreadId =
          mozilla::baseprofiler::profiler_current_thread_id();
      MOZ_RELEASE_ASSERT(testThreadId.IsSpecified());
      MOZ_RELEASE_ASSERT(testThreadId != mainThreadId);
      MOZ_RELEASE_ASSERT(!mozilla::baseprofiler::profiler_is_main_thread());
    });
    testThread.join();
  }

  // No conversions between processes and threads.
  static_assert(
      !std::is_constructible_v<mozilla::baseprofiler::BaseProfilerThreadId,
                               mozilla::baseprofiler::BaseProfilerProcessId>);
  static_assert(
      !std::is_assignable_v<mozilla::baseprofiler::BaseProfilerThreadId,
                            mozilla::baseprofiler::BaseProfilerProcessId>);
  static_assert(
      !std::is_constructible_v<mozilla::baseprofiler::BaseProfilerProcessId,
                               mozilla::baseprofiler::BaseProfilerThreadId>);
  static_assert(
      !std::is_assignable_v<mozilla::baseprofiler::BaseProfilerProcessId,
                            mozilla::baseprofiler::BaseProfilerThreadId>);

  printf("TestProfilerUtils done\n");
}

void TestBaseAndProfilerDetail() {
  printf("TestBaseAndProfilerDetail...\n");

  {
    using mozilla::profiler::detail::FilterHasPid;

    const auto pid123 =
        mozilla::baseprofiler::BaseProfilerProcessId::FromNumber(123);
    MOZ_RELEASE_ASSERT(FilterHasPid("pid:123", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid(" ", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("123", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid=123", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:123 ", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid: 123", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:0123", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:0000000000000000000000123", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:12", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:1234", pid123));
    MOZ_RELEASE_ASSERT(!FilterHasPid("pid:0", pid123));

    using PidNumber = mozilla::baseprofiler::BaseProfilerProcessId::NumberType;
    const PidNumber maxNumber = std::numeric_limits<PidNumber>::max();
    const auto maxPid =
        mozilla::baseprofiler::BaseProfilerProcessId::FromNumber(maxNumber);
    const std::string maxPidString = "pid:" + std::to_string(maxNumber);
    MOZ_RELEASE_ASSERT(FilterHasPid(maxPidString.c_str(), maxPid));

    const std::string tooBigPidString = maxPidString + "0";
    MOZ_RELEASE_ASSERT(!FilterHasPid(tooBigPidString.c_str(), maxPid));
  }

  {
    using mozilla::profiler::detail::FiltersExcludePid;
    const auto pid123 =
        mozilla::baseprofiler::BaseProfilerProcessId::FromNumber(123);

    MOZ_RELEASE_ASSERT(
        !FiltersExcludePid(mozilla::Span<const char*>{}, pid123));

    {
      const char* const filters[] = {"main"};
      MOZ_RELEASE_ASSERT(!FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"main", "pid:123"};
      MOZ_RELEASE_ASSERT(!FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"main", "pid:456"};
      MOZ_RELEASE_ASSERT(!FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"pid:123"};
      MOZ_RELEASE_ASSERT(!FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"pid:123", "pid:456"};
      MOZ_RELEASE_ASSERT(!FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"pid:456", "pid:123"};
      MOZ_RELEASE_ASSERT(!FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"pid:456"};
      MOZ_RELEASE_ASSERT(FiltersExcludePid(filters, pid123));
    }

    {
      const char* const filters[] = {"pid:456", "pid:789"};
      MOZ_RELEASE_ASSERT(FiltersExcludePid(filters, pid123));
    }
  }

  printf("TestBaseAndProfilerDetail done\n");
}

void TestSharedMutex() {
  printf("TestSharedMutex...\n");

  mozilla::baseprofiler::detail::BaseProfilerSharedMutex sm;

  // First round of minimal tests in this thread.

  MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());

  sm.LockExclusive();
  MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
  sm.UnlockExclusive();
  MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());

  sm.LockShared();
  MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
  sm.UnlockShared();
  MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());

  {
    mozilla::baseprofiler::detail::BaseProfilerAutoLockExclusive exclusiveLock{
        sm};
    MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
  }
  MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());

  {
    mozilla::baseprofiler::detail::BaseProfilerAutoLockShared sharedLock{sm};
    MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
  }
  MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());

  // The following will run actions between two threads, to verify that
  // exclusive and shared locks work as expected.

  // These actions will happen from top to bottom.
  // This will test all possible lock interactions.
  enum NextAction {                  // State of the lock:
    t1Starting,                      // (x=exclusive, s=shared, ?=blocked)
    t2Starting,                      // t1 t2
    t1LockExclusive,                 // x
    t2LockExclusiveAndBlock,         // x  x? - Can't have two exclusives.
    t1UnlockExclusive,               //    x
    t2UnblockedAfterT1Unlock,        //    x
    t1LockSharedAndBlock,            // s? x - Can't have shared during excl
    t2UnlockExclusive,               // s
    t1UnblockedAfterT2Unlock,        // s
    t2LockShared,                    // s  s - Can have multiple shared locks
    t1UnlockShared,                  //    s
    t2StillLockedShared,             //    s
    t1LockExclusiveAndBlock,         // x? s - Can't have excl during shared
    t2UnlockShared,                  // x
    t1UnblockedAfterT2UnlockShared,  // x
    t2CheckAfterT1Lock,              // x
    t1LastUnlockExclusive,           // (unlocked)
    done
  };

  // Each thread will repeatedly read this `nextAction`, and run actions that
  // target it...
  std::atomic<NextAction> nextAction{static_cast<NextAction>(0)};
  // ... and advance to the next available action (which should usually be for
  // the other thread).
  auto AdvanceAction = [&nextAction]() {
    MOZ_RELEASE_ASSERT(nextAction <= done);
    nextAction = static_cast<NextAction>(static_cast<int>(nextAction) + 1);
  };

  std::thread t1{[&]() {
    for (;;) {
      switch (nextAction) {
        case t1Starting:
          AdvanceAction();
          break;
        case t1LockExclusive:
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          sm.LockExclusive();
          MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
          AdvanceAction();
          break;
        case t1UnlockExclusive:
          MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
          // Advance first, before unlocking, so that t2 sees the new state.
          AdvanceAction();
          sm.UnlockExclusive();
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          break;
        case t1LockSharedAndBlock:
          // Advance action before attempting to lock after t2's exclusive lock.
          AdvanceAction();
          sm.LockShared();
          // We will only acquire the lock after t1 unlocks.
          MOZ_RELEASE_ASSERT(nextAction == t1UnblockedAfterT2Unlock);
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          AdvanceAction();
          break;
        case t1UnlockShared:
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          // Advance first, before unlocking, so that t2 sees the new state.
          AdvanceAction();
          sm.UnlockShared();
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          break;
        case t1LockExclusiveAndBlock:
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          // Advance action before attempting to lock after t2's shared lock.
          AdvanceAction();
          sm.LockExclusive();
          // We will only acquire the lock after t2 unlocks.
          MOZ_RELEASE_ASSERT(nextAction == t1UnblockedAfterT2UnlockShared);
          MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
          AdvanceAction();
          break;
        case t1LastUnlockExclusive:
          MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
          // Advance first, before unlocking, so that t2 sees the new state.
          AdvanceAction();
          sm.UnlockExclusive();
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          break;
        case done:
          return;
        default:
          // Ignore other actions intended for t2.
          break;
      }
    }
  }};

  std::thread t2{[&]() {
    for (;;) {
      switch (nextAction) {
        case t2Starting:
          AdvanceAction();
          break;
        case t2LockExclusiveAndBlock:
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          // Advance action before attempting to lock after t1's exclusive lock.
          AdvanceAction();
          sm.LockExclusive();
          // We will only acquire the lock after t1 unlocks.
          MOZ_RELEASE_ASSERT(nextAction == t2UnblockedAfterT1Unlock);
          MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
          AdvanceAction();
          break;
        case t2UnlockExclusive:
          MOZ_RELEASE_ASSERT(sm.IsLockedExclusiveOnCurrentThread());
          // Advance first, before unlocking, so that t1 sees the new state.
          AdvanceAction();
          sm.UnlockExclusive();
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          break;
        case t2LockShared:
          sm.LockShared();
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          AdvanceAction();
          break;
        case t2StillLockedShared:
          AdvanceAction();
          break;
        case t2UnlockShared:
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          // Advance first, before unlocking, so that t1 sees the new state.
          AdvanceAction();
          sm.UnlockShared();
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          break;
        case t2CheckAfterT1Lock:
          MOZ_RELEASE_ASSERT(!sm.IsLockedExclusiveOnCurrentThread());
          AdvanceAction();
          break;
        case done:
          return;
        default:
          // Ignore other actions intended for t1.
          break;
      }
    }
  }};

  t1.join();
  t2.join();

  printf("TestSharedMutex done\n");
}

void TestProportionValue() {
  printf("TestProportionValue...\n");

  using mozilla::ProportionValue;

#define STATIC_ASSERT_EQ(a, b) \
  static_assert((a) == (b));   \
  MOZ_RELEASE_ASSERT((a) == (b));

#define STATIC_ASSERT(e) STATIC_ASSERT_EQ(e, true)

  // Conversion from&to double.
  STATIC_ASSERT_EQ(ProportionValue().ToDouble(), 0.0);
  STATIC_ASSERT_EQ(ProportionValue(0.0).ToDouble(), 0.0);
  STATIC_ASSERT_EQ(ProportionValue(0.5).ToDouble(), 0.5);
  STATIC_ASSERT_EQ(ProportionValue(1.0).ToDouble(), 1.0);

  // Clamping.
  STATIC_ASSERT_EQ(
      ProportionValue(std::numeric_limits<double>::min()).ToDouble(), 0.0);
  STATIC_ASSERT_EQ(
      ProportionValue(std::numeric_limits<long double>::min()).ToDouble(), 0.0);
  STATIC_ASSERT_EQ(ProportionValue(-1.0).ToDouble(), 0.0);
  STATIC_ASSERT_EQ(ProportionValue(-0.01).ToDouble(), 0.0);
  STATIC_ASSERT_EQ(ProportionValue(-0.0).ToDouble(), 0.0);
  STATIC_ASSERT_EQ(ProportionValue(1.01).ToDouble(), 1.0);
  STATIC_ASSERT_EQ(
      ProportionValue(std::numeric_limits<double>::max()).ToDouble(), 1.0);

  // User-defined literal.
  {
    using namespace mozilla::literals::ProportionValue_literals;
    STATIC_ASSERT_EQ(0_pc, ProportionValue(0.0));
    STATIC_ASSERT_EQ(0._pc, ProportionValue(0.0));
    STATIC_ASSERT_EQ(50_pc, ProportionValue(0.5));
    STATIC_ASSERT_EQ(50._pc, ProportionValue(0.5));
    STATIC_ASSERT_EQ(100_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(100._pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(101_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(100.01_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(1000_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(1000._pc, ProportionValue(1.0));
  }
  {
    // ProportionValue_literals is an inline namespace of mozilla::literals, so
    // it's optional.
    using namespace mozilla::literals;
    STATIC_ASSERT_EQ(0_pc, ProportionValue(0.0));
    STATIC_ASSERT_EQ(0._pc, ProportionValue(0.0));
    STATIC_ASSERT_EQ(50_pc, ProportionValue(0.5));
    STATIC_ASSERT_EQ(50._pc, ProportionValue(0.5));
    STATIC_ASSERT_EQ(100_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(100._pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(101_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(100.01_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(1000_pc, ProportionValue(1.0));
    STATIC_ASSERT_EQ(1000._pc, ProportionValue(1.0));
  }

  // Invalid construction, conversion to double NaN.
  MOZ_RELEASE_ASSERT(std::isnan(ProportionValue::MakeInvalid().ToDouble()));

  using namespace mozilla::literals::ProportionValue_literals;

  // Conversion to&from underlying integral number.
  STATIC_ASSERT_EQ(
      ProportionValue::FromUnderlyingType((0_pc).ToUnderlyingType()).ToDouble(),
      0.0);
  STATIC_ASSERT_EQ(
      ProportionValue::FromUnderlyingType((50_pc).ToUnderlyingType())
          .ToDouble(),
      0.5);
  STATIC_ASSERT_EQ(
      ProportionValue::FromUnderlyingType((100_pc).ToUnderlyingType())
          .ToDouble(),
      1.0);
  STATIC_ASSERT(ProportionValue::FromUnderlyingType(
                    ProportionValue::MakeInvalid().ToUnderlyingType())
                    .IsInvalid());

  // IsExactlyZero.
  STATIC_ASSERT(ProportionValue().IsExactlyZero());
  STATIC_ASSERT((0_pc).IsExactlyZero());
  STATIC_ASSERT(!(50_pc).IsExactlyZero());
  STATIC_ASSERT(!(100_pc).IsExactlyZero());
  STATIC_ASSERT(!ProportionValue::MakeInvalid().IsExactlyZero());

  // IsExactlyOne.
  STATIC_ASSERT(!ProportionValue().IsExactlyOne());
  STATIC_ASSERT(!(0_pc).IsExactlyOne());
  STATIC_ASSERT(!(50_pc).IsExactlyOne());
  STATIC_ASSERT((100_pc).IsExactlyOne());
  STATIC_ASSERT(!ProportionValue::MakeInvalid().IsExactlyOne());

  // IsValid.
  STATIC_ASSERT(ProportionValue().IsValid());
  STATIC_ASSERT((0_pc).IsValid());
  STATIC_ASSERT((50_pc).IsValid());
  STATIC_ASSERT((100_pc).IsValid());
  STATIC_ASSERT(!ProportionValue::MakeInvalid().IsValid());

  // IsInvalid.
  STATIC_ASSERT(!ProportionValue().IsInvalid());
  STATIC_ASSERT(!(0_pc).IsInvalid());
  STATIC_ASSERT(!(50_pc).IsInvalid());
  STATIC_ASSERT(!(100_pc).IsInvalid());
  STATIC_ASSERT(ProportionValue::MakeInvalid().IsInvalid());

  // Addition.
  STATIC_ASSERT_EQ((0_pc + 0_pc).ToDouble(), 0.0);
  STATIC_ASSERT_EQ((0_pc + 100_pc).ToDouble(), 1.0);
  STATIC_ASSERT_EQ((100_pc + 0_pc).ToDouble(), 1.0);
  STATIC_ASSERT_EQ((100_pc + 100_pc).ToDouble(), 1.0);
  STATIC_ASSERT((ProportionValue::MakeInvalid() + 50_pc).IsInvalid());
  STATIC_ASSERT((50_pc + ProportionValue::MakeInvalid()).IsInvalid());

  // Subtraction.
  STATIC_ASSERT_EQ((0_pc - 0_pc).ToDouble(), 0.0);
  STATIC_ASSERT_EQ((0_pc - 100_pc).ToDouble(), 0.0);
  STATIC_ASSERT_EQ((100_pc - 0_pc).ToDouble(), 1.0);
  STATIC_ASSERT_EQ((100_pc - 100_pc).ToDouble(), 0.0);
  STATIC_ASSERT((ProportionValue::MakeInvalid() - 50_pc).IsInvalid());
  STATIC_ASSERT((50_pc - ProportionValue::MakeInvalid()).IsInvalid());

  // Multiplication.
  STATIC_ASSERT_EQ((0_pc * 0_pc).ToDouble(), 0.0);
  STATIC_ASSERT_EQ((0_pc * 100_pc).ToDouble(), 0.0);
  STATIC_ASSERT_EQ((50_pc * 50_pc).ToDouble(), 0.25);
  STATIC_ASSERT_EQ((50_pc * 100_pc).ToDouble(), 0.5);
  STATIC_ASSERT_EQ((100_pc * 50_pc).ToDouble(), 0.5);
  STATIC_ASSERT_EQ((100_pc * 0_pc).ToDouble(), 0.0);
  STATIC_ASSERT_EQ((100_pc * 100_pc).ToDouble(), 1.0);
  STATIC_ASSERT((ProportionValue::MakeInvalid() * 50_pc).IsInvalid());
  STATIC_ASSERT((50_pc * ProportionValue::MakeInvalid()).IsInvalid());

  // Division by a positive integer value.
  STATIC_ASSERT_EQ((100_pc / 1u).ToDouble(), 1.0);
  STATIC_ASSERT_EQ((100_pc / 2u).ToDouble(), 0.5);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(6u) / 2u).ToUnderlyingType(), 3u);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(5u) / 2u).ToUnderlyingType(), 2u);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(1u) / 2u).ToUnderlyingType(), 0u);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(0u) / 2u).ToUnderlyingType(), 0u);
  STATIC_ASSERT((100_pc / 0u).IsInvalid());
  STATIC_ASSERT((ProportionValue::MakeInvalid() / 2u).IsInvalid());

  // Multiplication by a positive integer value.
  STATIC_ASSERT_EQ((100_pc * 1u).ToDouble(), 1.0);
  STATIC_ASSERT_EQ((50_pc * 1u).ToDouble(), 0.5);
  STATIC_ASSERT_EQ((50_pc * 2u).ToDouble(), 1.0);
  STATIC_ASSERT_EQ((50_pc * 3u).ToDouble(), 1.0);  // Clamped.
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(1u) * 2u).ToUnderlyingType(), 2u);
  STATIC_ASSERT((ProportionValue::MakeInvalid() * 2u).IsInvalid());

  // Verifying PV - u < (PV / u) * u <= PV, with n=3, PV between 6 and 9 :
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(6u) / 3u).ToUnderlyingType(), 2u);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(7u) / 3u).ToUnderlyingType(), 2u);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(8u) / 3u).ToUnderlyingType(), 2u);
  STATIC_ASSERT_EQ(
      (ProportionValue::FromUnderlyingType(9u) / 3u).ToUnderlyingType(), 3u);

  // Direct comparisons.
  STATIC_ASSERT_EQ(0_pc, 0_pc);
  STATIC_ASSERT(0_pc == 0_pc);
  STATIC_ASSERT(!(0_pc == 100_pc));
  STATIC_ASSERT(0_pc != 100_pc);
  STATIC_ASSERT(!(0_pc != 0_pc));
  STATIC_ASSERT(0_pc < 100_pc);
  STATIC_ASSERT(!(0_pc < 0_pc));
  STATIC_ASSERT(0_pc <= 0_pc);
  STATIC_ASSERT(0_pc <= 100_pc);
  STATIC_ASSERT(!(100_pc <= 0_pc));
  STATIC_ASSERT(100_pc > 0_pc);
  STATIC_ASSERT(!(100_pc > 100_pc));
  STATIC_ASSERT(100_pc >= 0_pc);
  STATIC_ASSERT(100_pc >= 100_pc);
  STATIC_ASSERT(!(0_pc >= 100_pc));
  // 0.5 is binary-friendly, so we can double it and compare it exactly.
  STATIC_ASSERT_EQ(50_pc + 50_pc, 100_pc);

#undef STATIC_ASSERT_EQ

  printf("TestProportionValue done\n");
}

template <typename Arg0, typename... Args>
bool AreAllEqual(Arg0&& aArg0, Args&&... aArgs) {
  return ((aArg0 == aArgs) && ...);
}

void TestProgressLogger() {
  printf("TestProgressLogger...\n");

  using mozilla::ProgressLogger;
  using mozilla::ProportionValue;
  using namespace mozilla::literals::ProportionValue_literals;

  auto progressRefPtr = mozilla::MakeRefPtr<ProgressLogger::SharedProgress>();
  MOZ_RELEASE_ASSERT(progressRefPtr);
  MOZ_RELEASE_ASSERT(progressRefPtr->Progress().IsExactlyZero());

  {
    ProgressLogger pl(progressRefPtr, "Started", "All done");
    MOZ_RELEASE_ASSERT(progressRefPtr->Progress().IsExactlyZero());
    MOZ_RELEASE_ASSERT(pl.GetGlobalProgress().IsExactlyZero());
    MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(),
                                   pl.GetLastGlobalLocation(), "Started"));

    // At this top level, the scale is 1:1.
    pl.SetLocalProgress(10_pc, "Top 10%");
    MOZ_RELEASE_ASSERT(
        AreAllEqual(progressRefPtr->Progress(), pl.GetGlobalProgress(), 10_pc));
    MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(),
                                   pl.GetLastGlobalLocation(), "Top 10%"));

    pl.SetLocalProgress(0_pc, "Restarted");
    MOZ_RELEASE_ASSERT(
        AreAllEqual(progressRefPtr->Progress(), pl.GetGlobalProgress(), 0_pc));
    MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(),
                                   pl.GetLastGlobalLocation(), "Restarted"));

    {
      // Create a sub-logger for the whole global range. Notice that this is
      // moving the current progress back to 0.
      ProgressLogger plSub1 =
          pl.CreateSubLoggerFromTo(0_pc, "Sub1 started", 100_pc, "Sub1 ended");
      MOZ_RELEASE_ASSERT(progressRefPtr->Progress().IsExactlyZero());
      MOZ_RELEASE_ASSERT(pl.GetGlobalProgress().IsExactlyZero());
      MOZ_RELEASE_ASSERT(plSub1.GetGlobalProgress().IsExactlyZero());
      MOZ_RELEASE_ASSERT(AreAllEqual(
          progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
          plSub1.GetLastGlobalLocation(), "Sub1 started"));

      // At this level, the scale is still 1:1.
      plSub1.SetLocalProgress(10_pc, "Sub1 10%");
      MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->Progress(),
                                     pl.GetGlobalProgress(),
                                     plSub1.GetGlobalProgress(), 10_pc));
      MOZ_RELEASE_ASSERT(AreAllEqual(
          progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
          plSub1.GetLastGlobalLocation(), "Sub1 10%"));

      {
        // Create a sub-logger half the global range.
        //   0              0.25   0.375    0.5    0.625    0.75             1
        //   |---------------|-------|-------|-------|-------|---------------|
        // plSub2:           0      0.25    0.5     0.75     1
        ProgressLogger plSub2 = plSub1.CreateSubLoggerFromTo(
            25_pc, "Sub2 started", 75_pc, "Sub2 ended");
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->Progress(), pl.GetGlobalProgress(),
            plSub1.GetGlobalProgress(), plSub2.GetGlobalProgress(), 25_pc));
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
            plSub1.GetLastGlobalLocation(), plSub2.GetLastGlobalLocation(),
            "Sub2 started"));

        plSub2.SetLocalProgress(25_pc, "Sub2 25%");
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->Progress(), pl.GetGlobalProgress(),
            plSub1.GetGlobalProgress(), plSub2.GetGlobalProgress(), 37.5_pc));
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
            plSub1.GetLastGlobalLocation(), plSub2.GetLastGlobalLocation(),
            "Sub2 25%"));

        plSub2.SetLocalProgress(50_pc, "Sub2 50%");
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->Progress(), pl.GetGlobalProgress(),
            plSub1.GetGlobalProgress(), plSub2.GetGlobalProgress(), 50_pc));
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
            plSub1.GetLastGlobalLocation(), plSub2.GetLastGlobalLocation(),
            "Sub2 50%"));

        {
          // Create a sub-logger half the parent range.
          //   0              0.25   0.375    0.5    0.625    0.75             1
          //   |---------------|-------|-------|-------|-------|---------------|
          // plSub2:           0      0.25    0.5     0.75     1
          // plSub3:                           0      0.5      1
          ProgressLogger plSub3 = plSub2.CreateSubLoggerTo(
              "Sub3 started", 100_pc, ProgressLogger::NO_LOCATION_UPDATE);
          MOZ_RELEASE_ASSERT(AreAllEqual(
              progressRefPtr->Progress(), pl.GetGlobalProgress(),
              plSub1.GetGlobalProgress(), plSub2.GetGlobalProgress(),
              plSub3.GetGlobalProgress(), 50_pc));
          MOZ_RELEASE_ASSERT(AreAllEqual(
              progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
              plSub1.GetLastGlobalLocation(), plSub2.GetLastGlobalLocation(),
              plSub3.GetLastGlobalLocation(), "Sub3 started"));

          plSub3.SetLocalProgress(50_pc, "Sub3 50%");
          MOZ_RELEASE_ASSERT(AreAllEqual(
              progressRefPtr->Progress(), pl.GetGlobalProgress(),
              plSub1.GetGlobalProgress(), plSub2.GetGlobalProgress(),
              plSub3.GetGlobalProgress(), 62.5_pc));
          MOZ_RELEASE_ASSERT(AreAllEqual(
              progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
              plSub1.GetLastGlobalLocation(), plSub2.GetLastGlobalLocation(),
              plSub3.GetLastGlobalLocation(), "Sub3 50%"));
        }  // End of plSub3

        // When plSub3 ends, progress moves to its 100%, which is also plSub2's
        // 100%, which is plSub1's and the global progress of 75%
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->Progress(), pl.GetGlobalProgress(),
            plSub1.GetGlobalProgress(), plSub2.GetGlobalProgress(), 75_pc));
        // But location is still at the last explicit update.
        MOZ_RELEASE_ASSERT(AreAllEqual(
            progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
            plSub1.GetLastGlobalLocation(), plSub2.GetLastGlobalLocation(),
            "Sub3 50%"));
      }  // End of plSub2

      MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->Progress(),
                                     pl.GetGlobalProgress(),
                                     plSub1.GetGlobalProgress(), 75_pc));
      MOZ_RELEASE_ASSERT(AreAllEqual(
          progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
          plSub1.GetLastGlobalLocation(), "Sub2 ended"));
    }  // End of plSub1

    MOZ_RELEASE_ASSERT(progressRefPtr->Progress().IsExactlyOne());
    MOZ_RELEASE_ASSERT(pl.GetGlobalProgress().IsExactlyOne());
    MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(),
                                   pl.GetLastGlobalLocation(), "Sub1 ended"));

    const auto loopStart = 75_pc;
    const auto loopEnd = 87.5_pc;
    const uint32_t loopCount = 8;
    uint32_t expectedIndex = 0u;
    auto expectedIterationStart = loopStart;
    const auto iterationIncrement = (loopEnd - loopStart) / loopCount;
    for (auto&& [index, loopPL] : pl.CreateLoopSubLoggersFromTo(
             loopStart, loopEnd, loopCount, "looping...")) {
      MOZ_RELEASE_ASSERT(index == expectedIndex);
      ++expectedIndex;
      MOZ_RELEASE_ASSERT(
          AreAllEqual(progressRefPtr->Progress(), pl.GetGlobalProgress(),
                      loopPL.GetGlobalProgress(), expectedIterationStart));
      MOZ_RELEASE_ASSERT(AreAllEqual(
          progressRefPtr->LastLocation(), pl.GetLastGlobalLocation(),
          loopPL.GetLastGlobalLocation(), "looping..."));

      loopPL.SetLocalProgress(50_pc, "half");
      MOZ_RELEASE_ASSERT(loopPL.GetGlobalProgress() ==
                         expectedIterationStart + iterationIncrement / 2u);
      MOZ_RELEASE_ASSERT(
          AreAllEqual(progressRefPtr->Progress(), pl.GetGlobalProgress(),
                      loopPL.GetGlobalProgress(),
                      expectedIterationStart + iterationIncrement / 2u));
      MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(),
                                     pl.GetLastGlobalLocation(),
                                     loopPL.GetLastGlobalLocation(), "half"));

      expectedIterationStart = expectedIterationStart + iterationIncrement;
    }
    MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->Progress(),
                                   pl.GetGlobalProgress(),
                                   expectedIterationStart));
    MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(),
                                   pl.GetLastGlobalLocation(), "looping..."));
  }  // End of pl
  MOZ_RELEASE_ASSERT(progressRefPtr->Progress().IsExactlyOne());
  MOZ_RELEASE_ASSERT(AreAllEqual(progressRefPtr->LastLocation(), "All done"));

  printf("TestProgressLogger done\n");
}

#ifdef MOZ_GECKO_PROFILER

MOZ_MAYBE_UNUSED static void SleepMilli(unsigned aMilliseconds) {
#  if defined(_MSC_VER) || defined(__MINGW32__)
  Sleep(aMilliseconds);
#  else
  struct timespec ts = {/* .tv_sec */ static_cast<time_t>(aMilliseconds / 1000),
                        /* ts.tv_nsec */ long(aMilliseconds % 1000) * 1000000};
  struct timespec tr = {0, 0};
  while (nanosleep(&ts, &tr)) {
    if (errno == EINTR) {
      ts = tr;
    } else {
      printf("nanosleep() -> %s\n", strerror(errno));
      exit(1);
    }
  }
#  endif
}

MOZ_MAYBE_UNUSED static void WaitUntilTimeStampChanges(
    const mozilla::TimeStamp& aTimeStampToCompare = mozilla::TimeStamp::Now()) {
  while (aTimeStampToCompare == mozilla::TimeStamp::Now()) {
    SleepMilli(1);
  }
}

using namespace mozilla;

void TestPowerOfTwoMask() {
  printf("TestPowerOfTwoMask...\n");

  static_assert(MakePowerOfTwoMask<uint32_t, 0>().MaskValue() == 0);
  constexpr PowerOfTwoMask<uint32_t> c0 = MakePowerOfTwoMask<uint32_t, 0>();
  MOZ_RELEASE_ASSERT(c0.MaskValue() == 0);

  static_assert(MakePowerOfTwoMask<uint32_t, 0xFFu>().MaskValue() == 0xFFu);
  constexpr PowerOfTwoMask<uint32_t> cFF =
      MakePowerOfTwoMask<uint32_t, 0xFFu>();
  MOZ_RELEASE_ASSERT(cFF.MaskValue() == 0xFFu);

  static_assert(MakePowerOfTwoMask<uint32_t, 0xFFFFFFFFu>().MaskValue() ==
                0xFFFFFFFFu);
  constexpr PowerOfTwoMask<uint32_t> cFFFFFFFF =
      MakePowerOfTwoMask<uint32_t, 0xFFFFFFFFu>();
  MOZ_RELEASE_ASSERT(cFFFFFFFF.MaskValue() == 0xFFFFFFFFu);

  struct TestDataU32 {
    uint32_t mInput;
    uint32_t mMask;
  };
  // clang-format off
  TestDataU32 tests[] = {
    { 0, 0 },
    { 1, 1 },
    { 2, 3 },
    { 3, 3 },
    { 4, 7 },
    { 5, 7 },
    { (1u << 31) - 1, (1u << 31) - 1 },
    { (1u << 31), uint32_t(-1) },
    { (1u << 31) + 1, uint32_t(-1) },
    { uint32_t(-1), uint32_t(-1) }
  };
  // clang-format on
  for (const TestDataU32& test : tests) {
    PowerOfTwoMask<uint32_t> p2m(test.mInput);
    MOZ_RELEASE_ASSERT(p2m.MaskValue() == test.mMask);
    for (const TestDataU32& inner : tests) {
      if (p2m.MaskValue() != uint32_t(-1)) {
        MOZ_RELEASE_ASSERT((inner.mInput % p2m) ==
                           (inner.mInput % (p2m.MaskValue() + 1)));
      }
      MOZ_RELEASE_ASSERT((inner.mInput & p2m) == (inner.mInput % p2m));
      MOZ_RELEASE_ASSERT((p2m & inner.mInput) == (inner.mInput & p2m));
    }
  }

  printf("TestPowerOfTwoMask done\n");
}

void TestPowerOfTwo() {
  printf("TestPowerOfTwo...\n");

  static_assert(MakePowerOfTwo<uint32_t, 1>().Value() == 1);
  constexpr PowerOfTwo<uint32_t> c1 = MakePowerOfTwo<uint32_t, 1>();
  MOZ_RELEASE_ASSERT(c1.Value() == 1);
  static_assert(MakePowerOfTwo<uint32_t, 1>().Mask().MaskValue() == 0);

  static_assert(MakePowerOfTwo<uint32_t, 128>().Value() == 128);
  constexpr PowerOfTwo<uint32_t> c128 = MakePowerOfTwo<uint32_t, 128>();
  MOZ_RELEASE_ASSERT(c128.Value() == 128);
  static_assert(MakePowerOfTwo<uint32_t, 128>().Mask().MaskValue() == 127);

  static_assert(MakePowerOfTwo<uint32_t, 0x80000000u>().Value() == 0x80000000u);
  constexpr PowerOfTwo<uint32_t> cMax = MakePowerOfTwo<uint32_t, 0x80000000u>();
  MOZ_RELEASE_ASSERT(cMax.Value() == 0x80000000u);
  static_assert(MakePowerOfTwo<uint32_t, 0x80000000u>().Mask().MaskValue() ==
                0x7FFFFFFFu);

  struct TestDataU32 {
    uint32_t mInput;
    uint32_t mValue;
    uint32_t mMask;
  };
  // clang-format off
  TestDataU32 tests[] = {
    { 0, 1, 0 },
    { 1, 1, 0 },
    { 2, 2, 1 },
    { 3, 4, 3 },
    { 4, 4, 3 },
    { 5, 8, 7 },
    { (1u << 31) - 1, (1u << 31), (1u << 31) - 1 },
    { (1u << 31), (1u << 31), (1u << 31) - 1 },
    { (1u << 31) + 1, (1u << 31), (1u << 31) - 1 },
    { uint32_t(-1), (1u << 31), (1u << 31) - 1 }
  };
  // clang-format on
  for (const TestDataU32& test : tests) {
    PowerOfTwo<uint32_t> p2(test.mInput);
    MOZ_RELEASE_ASSERT(p2.Value() == test.mValue);
    MOZ_RELEASE_ASSERT(p2.MaskValue() == test.mMask);
    PowerOfTwoMask<uint32_t> p2m = p2.Mask();
    MOZ_RELEASE_ASSERT(p2m.MaskValue() == test.mMask);
    for (const TestDataU32& inner : tests) {
      MOZ_RELEASE_ASSERT((inner.mInput % p2) == (inner.mInput % p2.Value()));
    }
  }

  printf("TestPowerOfTwo done\n");
}

void TestLEB128() {
  printf("TestLEB128...\n");

  MOZ_RELEASE_ASSERT(ULEB128MaxSize<uint8_t>() == 2);
  MOZ_RELEASE_ASSERT(ULEB128MaxSize<uint16_t>() == 3);
  MOZ_RELEASE_ASSERT(ULEB128MaxSize<uint32_t>() == 5);
  MOZ_RELEASE_ASSERT(ULEB128MaxSize<uint64_t>() == 10);

  struct TestDataU64 {
    uint64_t mValue;
    unsigned mSize;
    const char* mBytes;
  };
  // clang-format off
  TestDataU64 tests[] = {
    // Small numbers should keep their normal byte representation.
    {                  0u,  1, "\0" },
    {                  1u,  1, "\x01" },

    // 0111 1111 (127, or 0x7F) is the highest number that fits into a single
    // LEB128 byte. It gets encoded as 0111 1111, note the most significant bit
    // is off.
    {               0x7Fu,  1, "\x7F" },

    // Next number: 128, or 0x80.
    //   Original data representation:  1000 0000
    //     Broken up into groups of 7:         1  0000000
    // Padded with 0 (msB) or 1 (lsB):  00000001 10000000
    //            Byte representation:  0x01     0x80
    //            Little endian order:  -> 0x80 0x01
    {               0x80u,  2, "\x80\x01" },

    // Next: 129, or 0x81 (showing that we don't lose low bits.)
    //   Original data representation:  1000 0001
    //     Broken up into groups of 7:         1  0000001
    // Padded with 0 (msB) or 1 (lsB):  00000001 10000001
    //            Byte representation:  0x01     0x81
    //            Little endian order:  -> 0x81 0x01
    {               0x81u,  2, "\x81\x01" },

    // Highest 8-bit number: 255, or 0xFF.
    //   Original data representation:  1111 1111
    //     Broken up into groups of 7:         1  1111111
    // Padded with 0 (msB) or 1 (lsB):  00000001 11111111
    //            Byte representation:  0x01     0xFF
    //            Little endian order:  -> 0xFF 0x01
    {               0xFFu,  2, "\xFF\x01" },

    // Next: 256, or 0x100.
    //   Original data representation:  1 0000 0000
    //     Broken up into groups of 7:        10  0000000
    // Padded with 0 (msB) or 1 (lsB):  00000010 10000000
    //            Byte representation:  0x10     0x80
    //            Little endian order:  -> 0x80 0x02
    {              0x100u,  2, "\x80\x02" },

    // Highest 32-bit number: 0xFFFFFFFF (8 bytes, all bits set).
    // Original: 1111 1111 1111 1111 1111 1111 1111 1111
    // Groups:     1111  1111111  1111111  1111111  1111111
    // Padded: 00001111 11111111 11111111 11111111 11111111
    // Bytes:  0x0F     0xFF     0xFF     0xFF     0xFF
    // Little Endian: -> 0xFF 0xFF 0xFF 0xFF 0x0F
    {         0xFFFFFFFFu,  5, "\xFF\xFF\xFF\xFF\x0F" },

    // Highest 64-bit number: 0xFFFFFFFFFFFFFFFF (16 bytes, all bits set).
    // 64 bits, that's 9 groups of 7 bits, plus 1 (most significant) bit.
    { 0xFFFFFFFFFFFFFFFFu, 10, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x01" }
  };
  // clang-format on

  for (const TestDataU64& test : tests) {
    MOZ_RELEASE_ASSERT(ULEB128Size(test.mValue) == test.mSize);
    // Prepare a buffer that can accomodate the largest-possible LEB128.
    uint8_t buffer[ULEB128MaxSize<uint64_t>()];
    // Use a pointer into the buffer as iterator.
    uint8_t* p = buffer;
    // And write the LEB128.
    WriteULEB128(test.mValue, p);
    // Pointer (iterator) should have advanced just past the expected LEB128
    // size.
    MOZ_RELEASE_ASSERT(p == buffer + test.mSize);
    // Check expected bytes.
    for (unsigned i = 0; i < test.mSize; ++i) {
      MOZ_RELEASE_ASSERT(buffer[i] == uint8_t(test.mBytes[i]));
    }

    // Move pointer (iterator) back to start of buffer.
    p = buffer;
    // And read the LEB128 we wrote above.
    uint64_t read = ReadULEB128<uint64_t>(p);
    // Pointer (iterator) should have also advanced just past the expected
    // LEB128 size.
    MOZ_RELEASE_ASSERT(p == buffer + test.mSize);
    // And check the read value.
    MOZ_RELEASE_ASSERT(read == test.mValue);

    // Testing ULEB128 reader.
    ULEB128Reader<uint64_t> reader;
    MOZ_RELEASE_ASSERT(!reader.IsComplete());
    // Move pointer back to start of buffer.
    p = buffer;
    for (;;) {
      // Read a byte and feed it to the reader.
      if (reader.FeedByteIsComplete(*p++)) {
        break;
      }
      // Not complete yet, we shouldn't have reached the end pointer.
      MOZ_RELEASE_ASSERT(!reader.IsComplete());
      MOZ_RELEASE_ASSERT(p < buffer + test.mSize);
    }
    MOZ_RELEASE_ASSERT(reader.IsComplete());
    // Pointer should have advanced just past the expected LEB128 size.
    MOZ_RELEASE_ASSERT(p == buffer + test.mSize);
    // And check the read value.
    MOZ_RELEASE_ASSERT(reader.Value() == test.mValue);

    // And again after a Reset.
    reader.Reset();
    MOZ_RELEASE_ASSERT(!reader.IsComplete());
    p = buffer;
    for (;;) {
      if (reader.FeedByteIsComplete(*p++)) {
        break;
      }
      MOZ_RELEASE_ASSERT(!reader.IsComplete());
      MOZ_RELEASE_ASSERT(p < buffer + test.mSize);
    }
    MOZ_RELEASE_ASSERT(reader.IsComplete());
    MOZ_RELEASE_ASSERT(p == buffer + test.mSize);
    MOZ_RELEASE_ASSERT(reader.Value() == test.mValue);
  }

  printf("TestLEB128 done\n");
}

struct StringWriteFunc final : public JSONWriteFunc {
  std::string mString;

  void Write(const mozilla::Span<const char>& aStr) final {
    mString.append(aStr.data(), aStr.size());
  }
};

void CheckJSON(mozilla::baseprofiler::SpliceableJSONWriter& aWriter,
               const char* aExpected, int aLine) {
  const std::string& actual =
      static_cast<StringWriteFunc&>(aWriter.WriteFunc()).mString;
  if (strcmp(aExpected, actual.c_str()) != 0) {
    fprintf(stderr,
            "---- EXPECTED ---- (line %d)\n<<<%s>>>\n"
            "---- ACTUAL ----\n<<<%s>>>\n",
            aLine, aExpected, actual.c_str());
    MOZ_RELEASE_ASSERT(false, "expected and actual output don't match");
  }
}

void TestJSONTimeOutput() {
  printf("TestJSONTimeOutput...\n");

#  define TEST(in, out)                                     \
    do {                                                    \
      mozilla::baseprofiler::SpliceableJSONWriter writer(   \
          mozilla::MakeUnique<StringWriteFunc>(),           \
          FailureLatchInfallibleSource::Singleton());       \
      writer.Start();                                       \
      writer.TimeDoubleMsProperty("time_ms", (in));         \
      writer.End();                                         \
      CheckJSON(writer, "{\"time_ms\":" out "}", __LINE__); \
    } while (false);

  TEST(0, "0");

  TEST(0.000'000'1, "0");
  TEST(0.000'000'4, "0");
  TEST(0.000'000'499, "0");
  TEST(0.000'000'5, "0.000001");
  TEST(0.000'001, "0.000001");
  TEST(0.000'01, "0.00001");
  TEST(0.000'1, "0.0001");
  TEST(0.001, "0.001");
  TEST(0.01, "0.01");
  TEST(0.1, "0.1");
  TEST(1, "1");
  TEST(2, "2");
  TEST(10, "10");
  TEST(100, "100");
  TEST(1'000, "1000");
  TEST(10'000, "10000");
  TEST(100'000, "100000");
  TEST(1'000'000, "1000000");
  // 2^53-2 ns in ms. 2^53-1 is the highest integer value representable in
  // double, -1 again because we're adding 0.5 before truncating.
  // That's 104 days, after which the nanosecond precision would decrease.
  TEST(9'007'199'254.740'990, "9007199254.74099");

  TEST(-0.000'000'1, "0");
  TEST(-0.000'000'4, "0");
  TEST(-0.000'000'499, "0");
  TEST(-0.000'000'5, "-0.000001");
  TEST(-0.000'001, "-0.000001");
  TEST(-0.000'01, "-0.00001");
  TEST(-0.000'1, "-0.0001");
  TEST(-0.001, "-0.001");
  TEST(-0.01, "-0.01");
  TEST(-0.1, "-0.1");
  TEST(-1, "-1");
  TEST(-2, "-2");
  TEST(-10, "-10");
  TEST(-100, "-100");
  TEST(-1'000, "-1000");
  TEST(-10'000, "-10000");
  TEST(-100'000, "-100000");
  TEST(-1'000'000, "-1000000");
  TEST(-9'007'199'254.740'990, "-9007199254.74099");

#  undef TEST

  printf("TestJSONTimeOutput done\n");
}

template <uint8_t byte, uint8_t... tail>
constexpr bool TestConstexprULEB128Reader(ULEB128Reader<uint64_t>& aReader) {
  if (aReader.IsComplete()) {
    return false;
  }
  const bool isComplete = aReader.FeedByteIsComplete(byte);
  if (aReader.IsComplete() != isComplete) {
    return false;
  }
  if constexpr (sizeof...(tail) == 0) {
    return isComplete;
  } else {
    if (isComplete) {
      return false;
    }
    return TestConstexprULEB128Reader<tail...>(aReader);
  }
}

template <uint64_t expected, uint8_t... bytes>
constexpr bool TestConstexprULEB128Reader() {
  ULEB128Reader<uint64_t> reader;
  if (!TestConstexprULEB128Reader<bytes...>(reader)) {
    return false;
  }
  if (!reader.IsComplete()) {
    return false;
  }
  if (reader.Value() != expected) {
    return false;
  }

  reader.Reset();
  if (!TestConstexprULEB128Reader<bytes...>(reader)) {
    return false;
  }
  if (!reader.IsComplete()) {
    return false;
  }
  if (reader.Value() != expected) {
    return false;
  }

  return true;
}

static_assert(TestConstexprULEB128Reader<0x0u, 0x0u>());
static_assert(!TestConstexprULEB128Reader<0x0u, 0x0u, 0x0u>());
static_assert(TestConstexprULEB128Reader<0x1u, 0x1u>());
static_assert(TestConstexprULEB128Reader<0x7Fu, 0x7Fu>());
static_assert(TestConstexprULEB128Reader<0x80u, 0x80u, 0x01u>());
static_assert(!TestConstexprULEB128Reader<0x80u, 0x80u>());
static_assert(!TestConstexprULEB128Reader<0x80u, 0x01u>());
static_assert(TestConstexprULEB128Reader<0x81u, 0x81u, 0x01u>());
static_assert(TestConstexprULEB128Reader<0xFFu, 0xFFu, 0x01u>());
static_assert(TestConstexprULEB128Reader<0x100u, 0x80u, 0x02u>());
static_assert(TestConstexprULEB128Reader<0xFFFFFFFFu, 0xFFu, 0xFFu, 0xFFu,
                                         0xFFu, 0x0Fu>());
static_assert(
    !TestConstexprULEB128Reader<0xFFFFFFFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu>());
static_assert(!TestConstexprULEB128Reader<0xFFFFFFFFu, 0xFFu, 0xFFu, 0xFFu,
                                          0xFFu, 0xFFu, 0x0Fu>());
static_assert(
    TestConstexprULEB128Reader<0xFFFFFFFFFFFFFFFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                               0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x01u>());
static_assert(
    !TestConstexprULEB128Reader<0xFFFFFFFFFFFFFFFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu>());

static void TestChunk() {
  printf("TestChunk...\n");

  static_assert(!std::is_default_constructible_v<ProfileBufferChunk>,
                "ProfileBufferChunk should not be default-constructible");
  static_assert(
      !std::is_constructible_v<ProfileBufferChunk, ProfileBufferChunk::Length>,
      "ProfileBufferChunk should not be constructible from Length");

  static_assert(
      sizeof(ProfileBufferChunk::Header) ==
          sizeof(ProfileBufferChunk::Header::mOffsetFirstBlock) +
              sizeof(ProfileBufferChunk::Header::mOffsetPastLastBlock) +
              sizeof(ProfileBufferChunk::Header::mStartTimeStamp) +
              sizeof(ProfileBufferChunk::Header::mDoneTimeStamp) +
              sizeof(ProfileBufferChunk::Header::mBufferBytes) +
              sizeof(ProfileBufferChunk::Header::mBlockCount) +
              sizeof(ProfileBufferChunk::Header::mRangeStart) +
              sizeof(ProfileBufferChunk::Header::mProcessId) +
              sizeof(ProfileBufferChunk::Header::mPADDING),
      "ProfileBufferChunk::Header may have unwanted padding, please review");
  // Note: The above static_assert is an attempt at keeping
  // ProfileBufferChunk::Header tightly packed, but some changes could make this
  // impossible to achieve (most probably due to alignment) -- Just do your
  // best!

  constexpr ProfileBufferChunk::Length TestLen = 1000;

  // Basic allocations of different sizes.
  for (ProfileBufferChunk::Length len = 0; len <= TestLen; ++len) {
    auto chunk = ProfileBufferChunk::Create(len);
    static_assert(
        std::is_same_v<decltype(chunk), UniquePtr<ProfileBufferChunk>>,
        "ProfileBufferChunk::Create() should return a "
        "UniquePtr<ProfileBufferChunk>");
    MOZ_RELEASE_ASSERT(!!chunk, "OOM!?");
    MOZ_RELEASE_ASSERT(chunk->BufferBytes() >= len);
    MOZ_RELEASE_ASSERT(chunk->ChunkBytes() >=
                       len + ProfileBufferChunk::SizeofChunkMetadata());
    MOZ_RELEASE_ASSERT(chunk->RemainingBytes() == chunk->BufferBytes());
    MOZ_RELEASE_ASSERT(chunk->OffsetFirstBlock() == 0);
    MOZ_RELEASE_ASSERT(chunk->OffsetPastLastBlock() == 0);
    MOZ_RELEASE_ASSERT(chunk->BlockCount() == 0);
    MOZ_RELEASE_ASSERT(chunk->ProcessId() == 0);
    MOZ_RELEASE_ASSERT(chunk->RangeStart() == 0);
    MOZ_RELEASE_ASSERT(chunk->BufferSpan().LengthBytes() ==
                       chunk->BufferBytes());
    MOZ_RELEASE_ASSERT(!chunk->GetNext());
    MOZ_RELEASE_ASSERT(!chunk->ReleaseNext());
    MOZ_RELEASE_ASSERT(chunk->Last() == chunk.get());
  }

  // Allocate the main test Chunk.
  auto chunkA = ProfileBufferChunk::Create(TestLen);
  MOZ_RELEASE_ASSERT(!!chunkA, "OOM!?");
  MOZ_RELEASE_ASSERT(chunkA->BufferBytes() >= TestLen);
  MOZ_RELEASE_ASSERT(chunkA->ChunkBytes() >=
                     TestLen + ProfileBufferChunk::SizeofChunkMetadata());
  MOZ_RELEASE_ASSERT(!chunkA->GetNext());
  MOZ_RELEASE_ASSERT(!chunkA->ReleaseNext());

  constexpr ProfileBufferIndex chunkARangeStart = 12345;
  chunkA->SetRangeStart(chunkARangeStart);
  MOZ_RELEASE_ASSERT(chunkA->RangeStart() == chunkARangeStart);

  // Get a read-only span over its buffer.
  auto bufferA = chunkA->BufferSpan();
  static_assert(
      std::is_same_v<decltype(bufferA), Span<const ProfileBufferChunk::Byte>>,
      "BufferSpan() should return a Span<const Byte>");
  MOZ_RELEASE_ASSERT(bufferA.LengthBytes() == chunkA->BufferBytes());

  // Add the initial tail block.
  constexpr ProfileBufferChunk::Length initTailLen = 10;
  auto initTail = chunkA->ReserveInitialBlockAsTail(initTailLen);
  static_assert(
      std::is_same_v<decltype(initTail), Span<ProfileBufferChunk::Byte>>,
      "ReserveInitialBlockAsTail() should return a Span<Byte>");
  MOZ_RELEASE_ASSERT(initTail.LengthBytes() == initTailLen);
  MOZ_RELEASE_ASSERT(initTail.Elements() == bufferA.Elements());
  MOZ_RELEASE_ASSERT(chunkA->OffsetFirstBlock() == initTailLen);
  MOZ_RELEASE_ASSERT(chunkA->OffsetPastLastBlock() == initTailLen);

  // Add the first complete block.
  constexpr ProfileBufferChunk::Length block1Len = 20;
  auto block1 = chunkA->ReserveBlock(block1Len);
  static_assert(
      std::is_same_v<decltype(block1), ProfileBufferChunk::ReserveReturn>,
      "ReserveBlock() should return a ReserveReturn");
  MOZ_RELEASE_ASSERT(block1.mBlockRangeIndex.ConvertToProfileBufferIndex() ==
                     chunkARangeStart + initTailLen);
  MOZ_RELEASE_ASSERT(block1.mSpan.LengthBytes() == block1Len);
  MOZ_RELEASE_ASSERT(block1.mSpan.Elements() ==
                     bufferA.Elements() + initTailLen);
  MOZ_RELEASE_ASSERT(chunkA->OffsetFirstBlock() == initTailLen);
  MOZ_RELEASE_ASSERT(chunkA->OffsetPastLastBlock() == initTailLen + block1Len);
  MOZ_RELEASE_ASSERT(chunkA->RemainingBytes() != 0);

  // Add another block to over-fill the ProfileBufferChunk.
  const ProfileBufferChunk::Length remaining =
      chunkA->BufferBytes() - (initTailLen + block1Len);
  constexpr ProfileBufferChunk::Length overfill = 30;
  const ProfileBufferChunk::Length block2Len = remaining + overfill;
  ProfileBufferChunk::ReserveReturn block2 = chunkA->ReserveBlock(block2Len);
  MOZ_RELEASE_ASSERT(block2.mBlockRangeIndex.ConvertToProfileBufferIndex() ==
                     chunkARangeStart + initTailLen + block1Len);
  MOZ_RELEASE_ASSERT(block2.mSpan.LengthBytes() == remaining);
  MOZ_RELEASE_ASSERT(block2.mSpan.Elements() ==
                     bufferA.Elements() + initTailLen + block1Len);
  MOZ_RELEASE_ASSERT(chunkA->OffsetFirstBlock() == initTailLen);
  MOZ_RELEASE_ASSERT(chunkA->OffsetPastLastBlock() == chunkA->BufferBytes());
  MOZ_RELEASE_ASSERT(chunkA->RemainingBytes() == 0);

  // Block must be marked "done" before it can be recycled.
  chunkA->MarkDone();

  // It must be marked "recycled" before data can be added to it again.
  chunkA->MarkRecycled();

  // Add an empty initial tail block.
  Span<ProfileBufferChunk::Byte> initTail2 =
      chunkA->ReserveInitialBlockAsTail(0);
  MOZ_RELEASE_ASSERT(initTail2.LengthBytes() == 0);
  MOZ_RELEASE_ASSERT(initTail2.Elements() == bufferA.Elements());
  MOZ_RELEASE_ASSERT(chunkA->OffsetFirstBlock() == 0);
  MOZ_RELEASE_ASSERT(chunkA->OffsetPastLastBlock() == 0);

  // Block must be marked "done" before it can be destroyed.
  chunkA->MarkDone();

  chunkA->SetProcessId(123);
  MOZ_RELEASE_ASSERT(chunkA->ProcessId() == 123);

  printf("TestChunk done\n");
}

static void TestChunkManagerSingle() {
  printf("TestChunkManagerSingle...\n");

  // Construct a ProfileBufferChunkManagerSingle for one chunk of size >=1000.
  constexpr ProfileBufferChunk::Length ChunkMinBufferBytes = 1000;
  ProfileBufferChunkManagerSingle cms{ChunkMinBufferBytes};

  // Reference to base class, to exercize virtual methods.
  ProfileBufferChunkManager& cm = cms;

#  ifdef DEBUG
  const char* chunkManagerRegisterer = "TestChunkManagerSingle";
  cm.RegisteredWith(chunkManagerRegisterer);
#  endif  // DEBUG

  const auto maxTotalSize = cm.MaxTotalSize();
  MOZ_RELEASE_ASSERT(maxTotalSize >= ChunkMinBufferBytes);

  cm.SetChunkDestroyedCallback([](const ProfileBufferChunk&) {
    MOZ_RELEASE_ASSERT(
        false,
        "ProfileBufferChunkManagerSingle should never destroy its one chunk");
  });

  UniquePtr<ProfileBufferChunk> extantReleasedChunks =
      cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");

  // First request.
  UniquePtr<ProfileBufferChunk> chunk = cm.GetChunk();
  MOZ_RELEASE_ASSERT(!!chunk, "First chunk request should always work");
  MOZ_RELEASE_ASSERT(chunk->BufferBytes() >= ChunkMinBufferBytes,
                     "Unexpected chunk size");
  MOZ_RELEASE_ASSERT(!chunk->GetNext(), "There should only be one chunk");

  // Keep address, for later checks.
  const uintptr_t chunkAddress = reinterpret_cast<uintptr_t>(chunk.get());

  extantReleasedChunks = cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");

  // Second request.
  MOZ_RELEASE_ASSERT(!cm.GetChunk(), "Second chunk request should always fail");

  extantReleasedChunks = cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");

  // Add some data to the chunk (to verify recycling later on).
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetFirstBlock == 0);
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetPastLastBlock == 0);
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 0);
  chunk->SetRangeStart(100);
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 100);
  Unused << chunk->ReserveInitialBlockAsTail(1);
  Unused << chunk->ReserveBlock(2);
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetFirstBlock == 1);
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetPastLastBlock == 1 + 2);

  // Release the first chunk.
  chunk->MarkDone();
  cm.ReleaseChunk(std::move(chunk));
  MOZ_RELEASE_ASSERT(!chunk, "chunk UniquePtr should have been moved-from");

  // Request after release.
  MOZ_RELEASE_ASSERT(!cm.GetChunk(),
                     "Chunk request after release should also fail");

  // Check released chunk.
  extantReleasedChunks = cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!!extantReleasedChunks,
                     "Could not retrieve released chunk");
  MOZ_RELEASE_ASSERT(!extantReleasedChunks->GetNext(),
                     "There should only be one released chunk");
  MOZ_RELEASE_ASSERT(
      reinterpret_cast<uintptr_t>(extantReleasedChunks.get()) == chunkAddress,
      "Released chunk should be first requested one");

  MOZ_RELEASE_ASSERT(!cm.GetExtantReleasedChunks(),
                     "Unexpected extra released chunk(s)");

  // Another request after release.
  MOZ_RELEASE_ASSERT(!cm.GetChunk(),
                     "Chunk request after release should also fail");

  MOZ_RELEASE_ASSERT(
      cm.MaxTotalSize() == maxTotalSize,
      "MaxTotalSize() should not change after requests&releases");

  // Reset the chunk manager. (Single-only non-virtual function.)
  cms.Reset(std::move(extantReleasedChunks));
  MOZ_RELEASE_ASSERT(!extantReleasedChunks,
                     "Released chunk UniquePtr should have been moved-from");

  MOZ_RELEASE_ASSERT(
      cm.MaxTotalSize() == maxTotalSize,
      "MaxTotalSize() should not change when resetting with the same chunk");

  // 2nd round, first request. Theoretically async, but this implementation just
  // immediately runs the callback.
  bool ran = false;
  cm.RequestChunk([&](UniquePtr<ProfileBufferChunk> aChunk) {
    ran = true;
    MOZ_RELEASE_ASSERT(!!aChunk);
    chunk = std::move(aChunk);
  });
  MOZ_RELEASE_ASSERT(ran, "RequestChunk callback not called immediately");
  ran = false;
  cm.FulfillChunkRequests();
  MOZ_RELEASE_ASSERT(!ran, "FulfillChunkRequests should not have any effects");
  MOZ_RELEASE_ASSERT(!!chunk, "First chunk request should always work");
  MOZ_RELEASE_ASSERT(chunk->BufferBytes() >= ChunkMinBufferBytes,
                     "Unexpected chunk size");
  MOZ_RELEASE_ASSERT(!chunk->GetNext(), "There should only be one chunk");
  MOZ_RELEASE_ASSERT(reinterpret_cast<uintptr_t>(chunk.get()) == chunkAddress,
                     "Requested chunk should be first requested one");
  // Verify that chunk is empty and usable.
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetFirstBlock == 0);
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetPastLastBlock == 0);
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 0);
  chunk->SetRangeStart(200);
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 200);
  Unused << chunk->ReserveInitialBlockAsTail(3);
  Unused << chunk->ReserveBlock(4);
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetFirstBlock == 3);
  MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetPastLastBlock == 3 + 4);

  // Second request.
  ran = false;
  cm.RequestChunk([&](UniquePtr<ProfileBufferChunk> aChunk) {
    ran = true;
    MOZ_RELEASE_ASSERT(!aChunk, "Second chunk request should always fail");
  });
  MOZ_RELEASE_ASSERT(ran, "RequestChunk callback not called");

  // This one does nothing.
  cm.ForgetUnreleasedChunks();

  // Don't forget to mark chunk "Done" before letting it die.
  chunk->MarkDone();
  chunk = nullptr;

  // Create a tiny chunk and reset the chunk manager with it.
  chunk = ProfileBufferChunk::Create(1);
  MOZ_RELEASE_ASSERT(!!chunk);
  auto tinyChunkSize = chunk->BufferBytes();
  MOZ_RELEASE_ASSERT(tinyChunkSize >= 1);
  MOZ_RELEASE_ASSERT(tinyChunkSize < ChunkMinBufferBytes);
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 0);
  chunk->SetRangeStart(300);
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 300);
  cms.Reset(std::move(chunk));
  MOZ_RELEASE_ASSERT(!chunk, "chunk UniquePtr should have been moved-from");
  MOZ_RELEASE_ASSERT(cm.MaxTotalSize() == tinyChunkSize,
                     "MaxTotalSize() should match the new chunk size");
  chunk = cm.GetChunk();
  MOZ_RELEASE_ASSERT(chunk->RangeStart() == 0, "Got non-recycled chunk");

  // Enough testing! Clean-up.
  Unused << chunk->ReserveInitialBlockAsTail(0);
  chunk->MarkDone();
  cm.ForgetUnreleasedChunks();

#  ifdef DEBUG
  cm.DeregisteredFrom(chunkManagerRegisterer);
#  endif  // DEBUG

  printf("TestChunkManagerSingle done\n");
}

static void TestChunkManagerWithLocalLimit() {
  printf("TestChunkManagerWithLocalLimit...\n");

  // Construct a ProfileBufferChunkManagerWithLocalLimit with chunk of minimum
  // size >=100, up to 1000 bytes.
  constexpr ProfileBufferChunk::Length MaxTotalBytes = 1000;
  constexpr ProfileBufferChunk::Length ChunkMinBufferBytes = 100;
  ProfileBufferChunkManagerWithLocalLimit cmll{MaxTotalBytes,
                                               ChunkMinBufferBytes};

  // Reference to base class, to exercize virtual methods.
  ProfileBufferChunkManager& cm = cmll;

#  ifdef DEBUG
  const char* chunkManagerRegisterer = "TestChunkManagerWithLocalLimit";
  cm.RegisteredWith(chunkManagerRegisterer);
#  endif  // DEBUG

  MOZ_RELEASE_ASSERT(cm.MaxTotalSize() == MaxTotalBytes,
                     "Max total size should be exactly as given");

  unsigned destroyedChunks = 0;
  unsigned destroyedBytes = 0;
  cm.SetChunkDestroyedCallback([&](const ProfileBufferChunk& aChunks) {
    for (const ProfileBufferChunk* chunk = &aChunks; chunk;
         chunk = chunk->GetNext()) {
      destroyedChunks += 1;
      destroyedBytes += chunk->BufferBytes();
    }
  });

  UniquePtr<ProfileBufferChunk> extantReleasedChunks =
      cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");

  // First request.
  UniquePtr<ProfileBufferChunk> chunk = cm.GetChunk();
  MOZ_RELEASE_ASSERT(!!chunk,
                     "First chunk immediate request should always work");
  const auto chunkActualBufferBytes = chunk->BufferBytes();
  MOZ_RELEASE_ASSERT(chunkActualBufferBytes >= ChunkMinBufferBytes,
                     "Unexpected chunk size");
  MOZ_RELEASE_ASSERT(!chunk->GetNext(), "There should only be one chunk");

  // Keep address, for later checks.
  const uintptr_t chunk1Address = reinterpret_cast<uintptr_t>(chunk.get());

  extantReleasedChunks = cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");

  // Verify that ReleaseChunk accepts zero chunks.
  cm.ReleaseChunk(nullptr);
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");

  // For this test, we need to be able to get at least 2 chunks without hitting
  // the limit. (If this failed, it wouldn't necessary be a problem with
  // ProfileBufferChunkManagerWithLocalLimit, fiddle with constants at the top
  // of this test.)
  MOZ_RELEASE_ASSERT(chunkActualBufferBytes < 2 * MaxTotalBytes);

  unsigned chunk1ReuseCount = 0;

  // We will do enough loops to go through the maximum size a number of times.
  const unsigned Rollovers = 3;
  const unsigned Loops = Rollovers * MaxTotalBytes / chunkActualBufferBytes;
  for (unsigned i = 0; i < Loops; ++i) {
    // Add some data to the chunk.
    MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetFirstBlock == 0);
    MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetPastLastBlock == 0);
    MOZ_RELEASE_ASSERT(chunk->RangeStart() == 0);
    const ProfileBufferIndex index = 1 + i * chunkActualBufferBytes;
    chunk->SetRangeStart(index);
    MOZ_RELEASE_ASSERT(chunk->RangeStart() == index);
    Unused << chunk->ReserveInitialBlockAsTail(1);
    Unused << chunk->ReserveBlock(2);
    MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetFirstBlock == 1);
    MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mOffsetPastLastBlock == 1 + 2);

    // Request a new chunk.
    bool ran = false;
    UniquePtr<ProfileBufferChunk> newChunk;
    cm.RequestChunk([&](UniquePtr<ProfileBufferChunk> aChunk) {
      ran = true;
      newChunk = std::move(aChunk);
    });
    MOZ_RELEASE_ASSERT(
        !ran, "RequestChunk should not immediately fulfill the request");
    cm.FulfillChunkRequests();
    MOZ_RELEASE_ASSERT(ran, "FulfillChunkRequests should invoke the callback");
    MOZ_RELEASE_ASSERT(!!newChunk, "Chunk request should always work");
    MOZ_RELEASE_ASSERT(newChunk->BufferBytes() == chunkActualBufferBytes,
                       "Unexpected chunk size");
    MOZ_RELEASE_ASSERT(!newChunk->GetNext(), "There should only be one chunk");

    // Mark previous chunk done and release it.
    WaitUntilTimeStampChanges();  // Force "done" timestamp to change.
    chunk->MarkDone();
    cm.ReleaseChunk(std::move(chunk));

    // And cycle to the new chunk.
    chunk = std::move(newChunk);

    if (reinterpret_cast<uintptr_t>(chunk.get()) == chunk1Address) {
      ++chunk1ReuseCount;
    }
  }

  // Expect all rollovers except 1 to destroy chunks.
  MOZ_RELEASE_ASSERT(destroyedChunks >= (Rollovers - 1) * MaxTotalBytes /
                                            chunkActualBufferBytes,
                     "Not enough destroyed chunks");
  MOZ_RELEASE_ASSERT(destroyedBytes == destroyedChunks * chunkActualBufferBytes,
                     "Mismatched destroyed chunks and bytes");
  MOZ_RELEASE_ASSERT(chunk1ReuseCount >= (Rollovers - 1),
                     "Not enough reuse of the first chunks");

  // Check that chunk manager is reentrant from request callback.
  bool ran = false;
  bool ranInner = false;
  UniquePtr<ProfileBufferChunk> newChunk;
  cm.RequestChunk([&](UniquePtr<ProfileBufferChunk> aChunk) {
    ran = true;
    MOZ_RELEASE_ASSERT(!!aChunk, "Chunk request should always work");
    Unused << aChunk->ReserveInitialBlockAsTail(0);
    WaitUntilTimeStampChanges();  // Force "done" timestamp to change.
    aChunk->MarkDone();
    UniquePtr<ProfileBufferChunk> anotherChunk = cm.GetChunk();
    MOZ_RELEASE_ASSERT(!!anotherChunk);
    Unused << anotherChunk->ReserveInitialBlockAsTail(0);
    WaitUntilTimeStampChanges();  // Force "done" timestamp to change.
    anotherChunk->MarkDone();
    cm.RequestChunk([&](UniquePtr<ProfileBufferChunk> aChunk) {
      ranInner = true;
      MOZ_RELEASE_ASSERT(!!aChunk, "Chunk request should always work");
      Unused << aChunk->ReserveInitialBlockAsTail(0);
      WaitUntilTimeStampChanges();  // Force "done" timestamp to change.
      aChunk->MarkDone();
    });
    MOZ_RELEASE_ASSERT(
        !ranInner, "RequestChunk should not immediately fulfill the request");
  });
  MOZ_RELEASE_ASSERT(!ran,
                     "RequestChunk should not immediately fulfill the request");
  MOZ_RELEASE_ASSERT(
      !ranInner,
      "RequestChunk should not immediately fulfill the inner request");
  cm.FulfillChunkRequests();
  MOZ_RELEASE_ASSERT(ran, "FulfillChunkRequests should invoke the callback");
  MOZ_RELEASE_ASSERT(!ranInner,
                     "FulfillChunkRequests should not immediately fulfill "
                     "the inner request");
  cm.FulfillChunkRequests();
  MOZ_RELEASE_ASSERT(
      ran, "2nd FulfillChunkRequests should invoke the inner request callback");

  // Enough testing! Clean-up.
  Unused << chunk->ReserveInitialBlockAsTail(0);
  WaitUntilTimeStampChanges();  // Force "done" timestamp to change.
  chunk->MarkDone();
  cm.ForgetUnreleasedChunks();

  // Special testing of the release algorithm, to make sure released chunks get
  // sorted.
  constexpr unsigned RandomReleaseChunkLoop = 100;
  // Build a vector of chunks, and mark them "done", ready to be released.
  Vector<UniquePtr<ProfileBufferChunk>> chunksToRelease;
  MOZ_RELEASE_ASSERT(chunksToRelease.reserve(RandomReleaseChunkLoop));
  Vector<TimeStamp> chunksTimeStamps;
  MOZ_RELEASE_ASSERT(chunksTimeStamps.reserve(RandomReleaseChunkLoop));
  for (unsigned i = 0; i < RandomReleaseChunkLoop; ++i) {
    UniquePtr<ProfileBufferChunk> chunk = cm.GetChunk();
    MOZ_RELEASE_ASSERT(chunk);
    Unused << chunk->ReserveInitialBlockAsTail(0);
    chunk->MarkDone();
    MOZ_RELEASE_ASSERT(!chunk->ChunkHeader().mDoneTimeStamp.IsNull());
    chunksTimeStamps.infallibleEmplaceBack(chunk->ChunkHeader().mDoneTimeStamp);
    chunksToRelease.infallibleEmplaceBack(std::move(chunk));
    if (i % 10 == 0) {
      // "Done" timestamps should *usually* increase, let's make extra sure some
      // timestamps are actually different.
      WaitUntilTimeStampChanges();
    }
  }
  // Shuffle the list.
  std::random_device randomDevice;
  std::mt19937 generator(randomDevice());
  std::shuffle(chunksToRelease.begin(), chunksToRelease.end(), generator);
  // And release chunks one by one, checking that the list of released chunks
  // is always sorted.
  printf("TestChunkManagerWithLocalLimit - Shuffle test timestamps:");
  for (unsigned i = 0; i < RandomReleaseChunkLoop; ++i) {
    printf(" %f", (chunksToRelease[i]->ChunkHeader().mDoneTimeStamp -
                   TimeStamp::ProcessCreation())
                      .ToMicroseconds());
    cm.ReleaseChunk(std::move(chunksToRelease[i]));
    cm.PeekExtantReleasedChunks([i](const ProfileBufferChunk* releasedChunks) {
      MOZ_RELEASE_ASSERT(releasedChunks);
      unsigned releasedChunkCount = 1;
      for (;;) {
        const ProfileBufferChunk* nextChunk = releasedChunks->GetNext();
        if (!nextChunk) {
          break;
        }
        ++releasedChunkCount;
        MOZ_RELEASE_ASSERT(releasedChunks->ChunkHeader().mDoneTimeStamp <=
                           nextChunk->ChunkHeader().mDoneTimeStamp);
        releasedChunks = nextChunk;
      }
      MOZ_RELEASE_ASSERT(releasedChunkCount == i + 1);
    });
  }
  printf("\n");
  // Finally, the whole list of released chunks should have the exact same
  // timestamps as the initial list of "done" chunks.
  extantReleasedChunks = cm.GetExtantReleasedChunks();
  for (unsigned i = 0; i < RandomReleaseChunkLoop; ++i) {
    MOZ_RELEASE_ASSERT(extantReleasedChunks, "Not enough released chunks");
    MOZ_RELEASE_ASSERT(extantReleasedChunks->ChunkHeader().mDoneTimeStamp ==
                       chunksTimeStamps[i]);
    Unused << std::exchange(extantReleasedChunks,
                            extantReleasedChunks->ReleaseNext());
  }
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Too many released chunks");

#  ifdef DEBUG
  cm.DeregisteredFrom(chunkManagerRegisterer);
#  endif  // DEBUG

  printf("TestChunkManagerWithLocalLimit done\n");
}

static bool IsSameMetadata(
    const ProfileBufferControlledChunkManager::ChunkMetadata& a1,
    const ProfileBufferControlledChunkManager::ChunkMetadata& a2) {
  return a1.mDoneTimeStamp == a2.mDoneTimeStamp &&
         a1.mBufferBytes == a2.mBufferBytes;
};

static bool IsSameUpdate(
    const ProfileBufferControlledChunkManager::Update& a1,
    const ProfileBufferControlledChunkManager::Update& a2) {
  // Final and not-an-update don't carry other data, so we can test these two
  // states first.
  if (a1.IsFinal() || a2.IsFinal()) {
    return a1.IsFinal() && a2.IsFinal();
  }
  if (a1.IsNotUpdate() || a2.IsNotUpdate()) {
    return a1.IsNotUpdate() && a2.IsNotUpdate();
  }

  // Here, both are "normal" udpates, check member variables:

  if (a1.UnreleasedBytes() != a2.UnreleasedBytes()) {
    return false;
  }
  if (a1.ReleasedBytes() != a2.ReleasedBytes()) {
    return false;
  }
  if (a1.OldestDoneTimeStamp() != a2.OldestDoneTimeStamp()) {
    return false;
  }
  if (a1.NewlyReleasedChunksRef().size() !=
      a2.NewlyReleasedChunksRef().size()) {
    return false;
  }
  for (unsigned i = 0; i < a1.NewlyReleasedChunksRef().size(); ++i) {
    if (!IsSameMetadata(a1.NewlyReleasedChunksRef()[i],
                        a2.NewlyReleasedChunksRef()[i])) {
      return false;
    }
  }
  return true;
}

static void TestControlledChunkManagerUpdate() {
  printf("TestControlledChunkManagerUpdate...\n");

  using Update = ProfileBufferControlledChunkManager::Update;

  // Default construction.
  Update update1;
  MOZ_RELEASE_ASSERT(update1.IsNotUpdate());
  MOZ_RELEASE_ASSERT(!update1.IsFinal());

  // Clear an already-cleared update.
  update1.Clear();
  MOZ_RELEASE_ASSERT(update1.IsNotUpdate());
  MOZ_RELEASE_ASSERT(!update1.IsFinal());

  // Final construction with nullptr.
  const Update final(nullptr);
  MOZ_RELEASE_ASSERT(final.IsFinal());
  MOZ_RELEASE_ASSERT(!final.IsNotUpdate());

  // Copy final to cleared.
  update1 = final;
  MOZ_RELEASE_ASSERT(update1.IsFinal());
  MOZ_RELEASE_ASSERT(!update1.IsNotUpdate());

  // Copy final to final.
  update1 = final;
  MOZ_RELEASE_ASSERT(update1.IsFinal());
  MOZ_RELEASE_ASSERT(!update1.IsNotUpdate());

  // Clear a final update.
  update1.Clear();
  MOZ_RELEASE_ASSERT(update1.IsNotUpdate());
  MOZ_RELEASE_ASSERT(!update1.IsFinal());

  // Move final to cleared.
  update1 = Update(nullptr);
  MOZ_RELEASE_ASSERT(update1.IsFinal());
  MOZ_RELEASE_ASSERT(!update1.IsNotUpdate());

  // Move final to final.
  update1 = Update(nullptr);
  MOZ_RELEASE_ASSERT(update1.IsFinal());
  MOZ_RELEASE_ASSERT(!update1.IsNotUpdate());

  // Move from not-an-update (effectively same as Clear).
  update1 = Update();
  MOZ_RELEASE_ASSERT(update1.IsNotUpdate());
  MOZ_RELEASE_ASSERT(!update1.IsFinal());

  auto CreateBiggerChunkAfter = [](const ProfileBufferChunk& aChunkToBeat) {
    while (TimeStamp::Now() <= aChunkToBeat.ChunkHeader().mDoneTimeStamp) {
      ::SleepMilli(1);
    }
    auto chunk = ProfileBufferChunk::Create(aChunkToBeat.BufferBytes() * 2);
    MOZ_RELEASE_ASSERT(!!chunk);
    MOZ_RELEASE_ASSERT(chunk->BufferBytes() >= aChunkToBeat.BufferBytes() * 2);
    Unused << chunk->ReserveInitialBlockAsTail(0);
    chunk->MarkDone();
    MOZ_RELEASE_ASSERT(chunk->ChunkHeader().mDoneTimeStamp >
                       aChunkToBeat.ChunkHeader().mDoneTimeStamp);
    return chunk;
  };

  update1 = Update(1, 2, nullptr, nullptr);

  // Create initial update with 2 released chunks and 1 unreleased chunk.
  auto released = ProfileBufferChunk::Create(10);
  ProfileBufferChunk* c1 = released.get();
  Unused << c1->ReserveInitialBlockAsTail(0);
  c1->MarkDone();

  released->SetLast(CreateBiggerChunkAfter(*c1));
  ProfileBufferChunk* c2 = c1->GetNext();

  auto unreleased = CreateBiggerChunkAfter(*c2);
  ProfileBufferChunk* c3 = unreleased.get();

  Update update2(c3->BufferBytes(), c1->BufferBytes() + c2->BufferBytes(), c1,
                 c1);
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update2,
      Update(c3->BufferBytes(), c1->BufferBytes() + c2->BufferBytes(),
             c1->ChunkHeader().mDoneTimeStamp,
             {{c1->ChunkHeader().mDoneTimeStamp, c1->BufferBytes()},
              {c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()}})));
  // Check every field, this time only, after that we'll trust that the
  // `SameUpdate` test will be enough.
  MOZ_RELEASE_ASSERT(!update2.IsNotUpdate());
  MOZ_RELEASE_ASSERT(!update2.IsFinal());
  MOZ_RELEASE_ASSERT(update2.UnreleasedBytes() == c3->BufferBytes());
  MOZ_RELEASE_ASSERT(update2.ReleasedBytes() ==
                     c1->BufferBytes() + c2->BufferBytes());
  MOZ_RELEASE_ASSERT(update2.OldestDoneTimeStamp() ==
                     c1->ChunkHeader().mDoneTimeStamp);
  MOZ_RELEASE_ASSERT(update2.NewlyReleasedChunksRef().size() == 2);
  MOZ_RELEASE_ASSERT(
      IsSameMetadata(update2.NewlyReleasedChunksRef()[0],
                     {c1->ChunkHeader().mDoneTimeStamp, c1->BufferBytes()}));
  MOZ_RELEASE_ASSERT(
      IsSameMetadata(update2.NewlyReleasedChunksRef()[1],
                     {c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()}));

  // Fold into not-an-update.
  update1.Fold(std::move(update2));
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update1,
      Update(c3->BufferBytes(), c1->BufferBytes() + c2->BufferBytes(),
             c1->ChunkHeader().mDoneTimeStamp,
             {{c1->ChunkHeader().mDoneTimeStamp, c1->BufferBytes()},
              {c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()}})));

  // Pretend nothing happened.
  update2 = Update(c3->BufferBytes(), c1->BufferBytes() + c2->BufferBytes(), c1,
                   nullptr);
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update2, Update(c3->BufferBytes(), c1->BufferBytes() + c2->BufferBytes(),
                      c1->ChunkHeader().mDoneTimeStamp, {})));
  update1.Fold(std::move(update2));
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update1,
      Update(c3->BufferBytes(), c1->BufferBytes() + c2->BufferBytes(),
             c1->ChunkHeader().mDoneTimeStamp,
             {{c1->ChunkHeader().mDoneTimeStamp, c1->BufferBytes()},
              {c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()}})));

  // Pretend there's a new unreleased chunk.
  c3->SetLast(CreateBiggerChunkAfter(*c3));
  ProfileBufferChunk* c4 = c3->GetNext();
  update2 = Update(c3->BufferBytes() + c4->BufferBytes(),
                   c1->BufferBytes() + c2->BufferBytes(), c1, nullptr);
  MOZ_RELEASE_ASSERT(
      IsSameUpdate(update2, Update(c3->BufferBytes() + c4->BufferBytes(),
                                   c1->BufferBytes() + c2->BufferBytes(),
                                   c1->ChunkHeader().mDoneTimeStamp, {})));
  update1.Fold(std::move(update2));
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update1,
      Update(c3->BufferBytes() + c4->BufferBytes(),
             c1->BufferBytes() + c2->BufferBytes(),
             c1->ChunkHeader().mDoneTimeStamp,
             {{c1->ChunkHeader().mDoneTimeStamp, c1->BufferBytes()},
              {c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()}})));

  // Pretend the first unreleased chunk c3 has been released.
  released->SetLast(std::exchange(unreleased, unreleased->ReleaseNext()));
  update2 =
      Update(c4->BufferBytes(),
             c1->BufferBytes() + c2->BufferBytes() + c3->BufferBytes(), c1, c3);
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update2,
      Update(c4->BufferBytes(),
             c1->BufferBytes() + c2->BufferBytes() + c3->BufferBytes(),
             c1->ChunkHeader().mDoneTimeStamp,
             {{c3->ChunkHeader().mDoneTimeStamp, c3->BufferBytes()}})));
  update1.Fold(std::move(update2));
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update1,
      Update(c4->BufferBytes(),
             c1->BufferBytes() + c2->BufferBytes() + c3->BufferBytes(),
             c1->ChunkHeader().mDoneTimeStamp,
             {{c1->ChunkHeader().mDoneTimeStamp, c1->BufferBytes()},
              {c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()},
              {c3->ChunkHeader().mDoneTimeStamp, c3->BufferBytes()}})));

  // Pretend c1 has been destroyed, so the oldest timestamp is now at c2.
  released = released->ReleaseNext();
  c1 = nullptr;
  update2 = Update(c4->BufferBytes(), c2->BufferBytes() + c3->BufferBytes(), c2,
                   nullptr);
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update2, Update(c4->BufferBytes(), c2->BufferBytes() + c3->BufferBytes(),
                      c2->ChunkHeader().mDoneTimeStamp, {})));
  update1.Fold(std::move(update2));
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update1,
      Update(c4->BufferBytes(), c2->BufferBytes() + c3->BufferBytes(),
             c2->ChunkHeader().mDoneTimeStamp,
             {{c2->ChunkHeader().mDoneTimeStamp, c2->BufferBytes()},
              {c3->ChunkHeader().mDoneTimeStamp, c3->BufferBytes()}})));

  // Pretend c2 has been recycled to make unreleased c5, and c4 has been
  // released.
  auto recycled = std::exchange(released, released->ReleaseNext());
  recycled->MarkRecycled();
  Unused << recycled->ReserveInitialBlockAsTail(0);
  recycled->MarkDone();
  released->SetLast(std::move(unreleased));
  unreleased = std::move(recycled);
  ProfileBufferChunk* c5 = c2;
  c2 = nullptr;
  update2 =
      Update(c5->BufferBytes(), c3->BufferBytes() + c4->BufferBytes(), c3, c4);
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update2,
      Update(c5->BufferBytes(), c3->BufferBytes() + c4->BufferBytes(),
             c3->ChunkHeader().mDoneTimeStamp,
             {{c4->ChunkHeader().mDoneTimeStamp, c4->BufferBytes()}})));
  update1.Fold(std::move(update2));
  MOZ_RELEASE_ASSERT(IsSameUpdate(
      update1,
      Update(c5->BufferBytes(), c3->BufferBytes() + c4->BufferBytes(),
             c3->ChunkHeader().mDoneTimeStamp,
             {{c3->ChunkHeader().mDoneTimeStamp, c3->BufferBytes()},
              {c4->ChunkHeader().mDoneTimeStamp, c4->BufferBytes()}})));

  // And send a final update.
  update1.Fold(Update(nullptr));
  MOZ_RELEASE_ASSERT(update1.IsFinal());
  MOZ_RELEASE_ASSERT(!update1.IsNotUpdate());

  printf("TestControlledChunkManagerUpdate done\n");
}

static void TestControlledChunkManagerWithLocalLimit() {
  printf("TestControlledChunkManagerWithLocalLimit...\n");

  // Construct a ProfileBufferChunkManagerWithLocalLimit with chunk of minimum
  // size >=100, up to 1000 bytes.
  constexpr ProfileBufferChunk::Length MaxTotalBytes = 1000;
  constexpr ProfileBufferChunk::Length ChunkMinBufferBytes = 100;
  ProfileBufferChunkManagerWithLocalLimit cmll{MaxTotalBytes,
                                               ChunkMinBufferBytes};

  // Reference to chunk manager base class.
  ProfileBufferChunkManager& cm = cmll;

  // Reference to controlled chunk manager base class.
  ProfileBufferControlledChunkManager& ccm = cmll;

#  ifdef DEBUG
  const char* chunkManagerRegisterer =
      "TestControlledChunkManagerWithLocalLimit";
  cm.RegisteredWith(chunkManagerRegisterer);
#  endif  // DEBUG

  MOZ_RELEASE_ASSERT(cm.MaxTotalSize() == MaxTotalBytes,
                     "Max total size should be exactly as given");

  unsigned destroyedChunks = 0;
  unsigned destroyedBytes = 0;
  cm.SetChunkDestroyedCallback([&](const ProfileBufferChunk& aChunks) {
    for (const ProfileBufferChunk* chunk = &aChunks; chunk;
         chunk = chunk->GetNext()) {
      destroyedChunks += 1;
      destroyedBytes += chunk->BufferBytes();
    }
  });

  using Update = ProfileBufferControlledChunkManager::Update;
  unsigned updateCount = 0;
  ProfileBufferControlledChunkManager::Update update;
  MOZ_RELEASE_ASSERT(update.IsNotUpdate());
  auto updateCallback = [&](Update&& aUpdate) {
    ++updateCount;
    update.Fold(std::move(aUpdate));
  };
  ccm.SetUpdateCallback(updateCallback);
  MOZ_RELEASE_ASSERT(updateCount == 1,
                     "SetUpdateCallback should have triggered an update");
  MOZ_RELEASE_ASSERT(IsSameUpdate(update, Update(0, 0, TimeStamp{}, {})));
  updateCount = 0;
  update.Clear();

  UniquePtr<ProfileBufferChunk> extantReleasedChunks =
      cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");
  MOZ_RELEASE_ASSERT(updateCount == 1,
                     "GetExtantReleasedChunks should have triggered an update");
  MOZ_RELEASE_ASSERT(IsSameUpdate(update, Update(0, 0, TimeStamp{}, {})));
  updateCount = 0;
  update.Clear();

  // First request.
  UniquePtr<ProfileBufferChunk> chunk = cm.GetChunk();
  MOZ_RELEASE_ASSERT(!!chunk,
                     "First chunk immediate request should always work");
  const auto chunkActualBufferBytes = chunk->BufferBytes();
  MOZ_RELEASE_ASSERT(updateCount == 1,
                     "GetChunk should have triggered an update");
  MOZ_RELEASE_ASSERT(
      IsSameUpdate(update, Update(chunk->BufferBytes(), 0, TimeStamp{}, {})));
  updateCount = 0;
  update.Clear();

  extantReleasedChunks = cm.GetExtantReleasedChunks();
  MOZ_RELEASE_ASSERT(!extantReleasedChunks, "Unexpected released chunk(s)");
  MOZ_RELEASE_ASSERT(updateCount == 1,
                     "GetExtantReleasedChunks should have triggered an update");
  MOZ_RELEASE_ASSERT(
      IsSameUpdate(update, Update(chunk->BufferBytes(), 0, TimeStamp{}, {})));
  updateCount = 0;
  update.Clear();

  // For this test, we need to be able to get at least 2 chunks without hitting
  // the limit. (If this failed, it wouldn't necessary be a problem with
  // ProfileBufferChunkManagerWithLocalLimit, fiddle with constants at the top
  // of this test.)
  MOZ_RELEASE_ASSERT(chunkActualBufferBytes < 2 * MaxTotalBytes);

  ProfileBufferChunk::Length previousUnreleasedBytes = chunk->BufferBytes();
  ProfileBufferChunk::Length previousReleasedBytes = 0;
  TimeStamp previousOldestDoneTimeStamp;

  // We will do enough loops to go through the maximum size a number of times.
  const unsigned Rollovers = 3;
  const unsigned Loops = Rollovers * MaxTotalBytes / chunkActualBufferBytes;
  for (unsigned i = 0; i < Loops; ++i) {
    // Add some data to the chunk.
    const ProfileBufferIndex index =
        ProfileBufferIndex(chunkActualBufferBytes) * i + 1;
    chunk->SetRangeStart(index);
    Unused << chunk->ReserveInitialBlockAsTail(1);
    Unused << chunk->ReserveBlock(2);

    // Request a new chunk.
    UniquePtr<ProfileBufferChunk> newChunk;
    cm.RequestChunk([&](UniquePtr<ProfileBufferChunk> aChunk) {
      newChunk = std::move(aChunk);
    });
    MOZ_RELEASE_ASSERT(updateCount == 0,
                       "RequestChunk() shouldn't have triggered an update");
    cm.FulfillChunkRequests();
    MOZ_RELEASE_ASSERT(!!newChunk, "Chunk request should always work");
    MOZ_RELEASE_ASSERT(newChunk->BufferBytes() == chunkActualBufferBytes,
                       "Unexpected chunk size");
    MOZ_RELEASE_ASSERT(!newChunk->GetNext(), "There should only be one chunk");

    MOZ_RELEASE_ASSERT(updateCount == 1,
                       "FulfillChunkRequests() after a request should have "
                       "triggered an update");
    MOZ_RELEASE_ASSERT(!update.IsFinal());
    MOZ_RELEASE_ASSERT(!update.IsNotUpdate());
    MOZ_RELEASE_ASSERT(update.UnreleasedBytes() ==
                       previousUnreleasedBytes + newChunk->BufferBytes());
    previousUnreleasedBytes = update.UnreleasedBytes();
    MOZ_RELEASE_ASSERT(update.ReleasedBytes() <= previousReleasedBytes);
    previousReleasedBytes = update.ReleasedBytes();
    MOZ_RELEASE_ASSERT(previousOldestDoneTimeStamp.IsNull() ||
                       update.OldestDoneTimeStamp() >=
                           previousOldestDoneTimeStamp);
    previousOldestDoneTimeStamp = update.OldestDoneTimeStamp();
    MOZ_RELEASE_ASSERT(update.NewlyReleasedChunksRef().empty());
    updateCount = 0;
    update.Clear();

    // Make sure the "Done" timestamp below cannot be the same as from the
    // previous loop.
    const TimeStamp now = TimeStamp::Now();
    while (TimeStamp::Now() == now) {
      ::SleepMilli(1);
    }

    // Mark previous chunk done and release it.
    WaitUntilTimeStampChanges();  // Force "done" timestamp to change.
    chunk->MarkDone();
    const auto doneTimeStamp = chunk->ChunkHeader().mDoneTimeStamp;
    const auto bufferBytes = chunk->BufferBytes();
    cm.ReleaseChunk(std::move(chunk));

    MOZ_RELEASE_ASSERT(updateCount == 1,
                       "ReleaseChunk() should have triggered an update");
    MOZ_RELEASE_ASSERT(!update.IsFinal());
    MOZ_RELEASE_ASSERT(!update.IsNotUpdate());
    MOZ_RELEASE_ASSERT(update.UnreleasedBytes() ==
                       previousUnreleasedBytes - bufferBytes);
    previousUnreleasedBytes = update.UnreleasedBytes();
    MOZ_RELEASE_ASSERT(update.ReleasedBytes() ==
                       previousReleasedBytes + bufferBytes);
    previousReleasedBytes = update.ReleasedBytes();
    MOZ_RELEASE_ASSERT(previousOldestDoneTimeStamp.IsNull() ||
                       update.OldestDoneTimeStamp() >=
                           previousOldestDoneTimeStamp);
    previousOldestDoneTimeStamp = update.OldestDoneTimeStamp();
    MOZ_RELEASE_ASSERT(update.OldestDoneTimeStamp() <= doneTimeStamp);
    MOZ_RELEASE_ASSERT(update.NewlyReleasedChunksRef().size() == 1);
    MOZ_RELEASE_ASSERT(update.NewlyReleasedChunksRef()[0].mDoneTimeStamp ==
                       doneTimeStamp);
    MOZ_RELEASE_ASSERT(update.NewlyReleasedChunksRef()[0].mBufferBytes ==
                       bufferBytes);
    updateCount = 0;
    update.Clear();

    // And cycle to the new chunk.
    chunk = std::move(newChunk);
  }

  // Enough testing! Clean-up.
  Unused << chunk->ReserveInitialBlockAsTail(0);
  chunk->MarkDone();
  cm.ForgetUnreleasedChunks();
  MOZ_RELEASE_ASSERT(
      updateCount == 1,
      "ForgetUnreleasedChunks() should have triggered an update");
  MOZ_RELEASE_ASSERT(!update.IsFinal());
  MOZ_RELEASE_ASSERT(!update.IsNotUpdate());
  MOZ_RELEASE_ASSERT(update.UnreleasedBytes() == 0);
  MOZ_RELEASE_ASSERT(update.ReleasedBytes() == previousReleasedBytes);
  MOZ_RELEASE_ASSERT(update.NewlyReleasedChunksRef().empty() == 1);
  updateCount = 0;
  update.Clear();

  ccm.SetUpdateCallback({});
  MOZ_RELEASE_ASSERT(updateCount == 1,
                     "SetUpdateCallback({}) should have triggered an update");
  MOZ_RELEASE_ASSERT(update.IsFinal());

#  ifdef DEBUG
  cm.DeregisteredFrom(chunkManagerRegisterer);
#  endif  // DEBUG

  printf("TestControlledChunkManagerWithLocalLimit done\n");
}

#  define VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(                         \
      aProfileChunkedBuffer, aStart, aEnd, aPushed, aCleared, aFailed)        \
    {                                                                         \
      ProfileChunkedBuffer::State state = (aProfileChunkedBuffer).GetState(); \
      MOZ_RELEASE_ASSERT(state.mRangeStart == (aStart));                      \
      MOZ_RELEASE_ASSERT(state.mRangeEnd == (aEnd));                          \
      MOZ_RELEASE_ASSERT(state.mPushedBlockCount == (aPushed));               \
      MOZ_RELEASE_ASSERT(state.mClearedBlockCount == (aCleared));             \
      MOZ_RELEASE_ASSERT(state.mFailedPutBytes == (aFailed));                 \
    }

static void TestChunkedBuffer() {
  printf("TestChunkedBuffer...\n");

  ProfileBufferBlockIndex blockIndex;
  MOZ_RELEASE_ASSERT(!blockIndex);
  MOZ_RELEASE_ASSERT(blockIndex == nullptr);

  // Create an out-of-session ProfileChunkedBuffer.
  ProfileChunkedBuffer cb(ProfileChunkedBuffer::ThreadSafety::WithMutex);

  MOZ_RELEASE_ASSERT(cb.BufferLength().isNothing());

  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  int result = 0;
  result = cb.ReserveAndPut(
      []() {
        MOZ_RELEASE_ASSERT(false);
        return 1;
      },
      [](Maybe<ProfileBufferEntryWriter>& aEW) { return aEW ? 2 : 3; });
  MOZ_RELEASE_ASSERT(result == 3);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  result = 0;
  result = cb.Put(
      1, [](Maybe<ProfileBufferEntryWriter>& aEW) { return aEW ? 1 : 2; });
  MOZ_RELEASE_ASSERT(result == 2);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  blockIndex = cb.PutFrom(&result, 1);
  MOZ_RELEASE_ASSERT(!blockIndex);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  blockIndex = cb.PutObjects(123, result, "hello");
  MOZ_RELEASE_ASSERT(!blockIndex);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  blockIndex = cb.PutObject(123);
  MOZ_RELEASE_ASSERT(!blockIndex);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  auto chunks = cb.GetAllChunks();
  static_assert(std::is_same_v<decltype(chunks), UniquePtr<ProfileBufferChunk>>,
                "ProfileChunkedBuffer::GetAllChunks() should return a "
                "UniquePtr<ProfileBufferChunk>");
  MOZ_RELEASE_ASSERT(!chunks, "Expected no chunks when out-of-session");

  bool ran = false;
  result = 0;
  result = cb.Read([&](ProfileChunkedBuffer::Reader* aReader) {
    ran = true;
    MOZ_RELEASE_ASSERT(!aReader);
    return 3;
  });
  MOZ_RELEASE_ASSERT(ran);
  MOZ_RELEASE_ASSERT(result == 3);

  cb.ReadEach([](ProfileBufferEntryReader&) { MOZ_RELEASE_ASSERT(false); });

  result = 0;
  result = cb.ReadAt(nullptr, [](Maybe<ProfileBufferEntryReader>&& er) {
    MOZ_RELEASE_ASSERT(er.isNothing());
    return 4;
  });
  MOZ_RELEASE_ASSERT(result == 4);

  // Use ProfileBufferChunkManagerWithLocalLimit, which will give away
  // ProfileBufferChunks that can contain 128 bytes, using up to 1KB of memory
  // (including usable 128 bytes and headers).
  constexpr size_t bufferMaxSize = 1024;
  constexpr ProfileChunkedBuffer::Length chunkMinSize = 128;
  ProfileBufferChunkManagerWithLocalLimit cm(bufferMaxSize, chunkMinSize);
  cb.SetChunkManager(cm);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  // Let the chunk manager fulfill the initial request for an extra chunk.
  cm.FulfillChunkRequests();

  MOZ_RELEASE_ASSERT(cm.MaxTotalSize() == bufferMaxSize);
  MOZ_RELEASE_ASSERT(cb.BufferLength().isSome());
  MOZ_RELEASE_ASSERT(*cb.BufferLength() == bufferMaxSize);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1, 1, 0, 0, 0);

  // Write an int with the main `ReserveAndPut` function.
  const int test = 123;
  ran = false;
  blockIndex = nullptr;
  bool success = cb.ReserveAndPut(
      []() { return sizeof(test); },
      [&](Maybe<ProfileBufferEntryWriter>& aEW) {
        ran = true;
        if (!aEW) {
          return false;
        }
        blockIndex = aEW->CurrentBlockIndex();
        MOZ_RELEASE_ASSERT(aEW->RemainingBytes() == sizeof(test));
        aEW->WriteObject(test);
        MOZ_RELEASE_ASSERT(aEW->RemainingBytes() == 0);
        return true;
      });
  MOZ_RELEASE_ASSERT(ran);
  MOZ_RELEASE_ASSERT(success);
  MOZ_RELEASE_ASSERT(blockIndex.ConvertToProfileBufferIndex() == 1);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cb, 1, 1 + ULEB128Size(sizeof(test)) + sizeof(test), 1, 0, 0);

  ran = false;
  result = 0;
  result = cb.Read([&](ProfileChunkedBuffer::Reader* aReader) {
    ran = true;
    MOZ_RELEASE_ASSERT(!!aReader);
    // begin() and end() should be at the range edges (verified above).
    MOZ_RELEASE_ASSERT(
        aReader->begin().CurrentBlockIndex().ConvertToProfileBufferIndex() ==
        1);
    MOZ_RELEASE_ASSERT(
        aReader->end().CurrentBlockIndex().ConvertToProfileBufferIndex() == 0);
    // Null ProfileBufferBlockIndex clamped to the beginning.
    MOZ_RELEASE_ASSERT(aReader->At(nullptr) == aReader->begin());
    MOZ_RELEASE_ASSERT(aReader->At(blockIndex) == aReader->begin());
    // At(begin) same as begin().
    MOZ_RELEASE_ASSERT(aReader->At(aReader->begin().CurrentBlockIndex()) ==
                       aReader->begin());
    // At(past block) same as end().
    MOZ_RELEASE_ASSERT(
        aReader->At(ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
            1 + 1 + sizeof(test))) == aReader->end());

    size_t read = 0;
    aReader->ForEach([&](ProfileBufferEntryReader& er) {
      ++read;
      MOZ_RELEASE_ASSERT(er.RemainingBytes() == sizeof(test));
      const auto value = er.ReadObject<decltype(test)>();
      MOZ_RELEASE_ASSERT(value == test);
      MOZ_RELEASE_ASSERT(er.RemainingBytes() == 0);
    });
    MOZ_RELEASE_ASSERT(read == 1);

    read = 0;
    for (auto er : *aReader) {
      static_assert(std::is_same_v<decltype(er), ProfileBufferEntryReader>,
                    "ProfileChunkedBuffer::Reader range-for should produce "
                    "ProfileBufferEntryReader objects");
      ++read;
      MOZ_RELEASE_ASSERT(er.RemainingBytes() == sizeof(test));
      const auto value = er.ReadObject<decltype(test)>();
      MOZ_RELEASE_ASSERT(value == test);
      MOZ_RELEASE_ASSERT(er.RemainingBytes() == 0);
    };
    MOZ_RELEASE_ASSERT(read == 1);
    return 5;
  });
  MOZ_RELEASE_ASSERT(ran);
  MOZ_RELEASE_ASSERT(result == 5);

  // Read the int directly from the ProfileChunkedBuffer, without block index.
  size_t read = 0;
  cb.ReadEach([&](ProfileBufferEntryReader& er) {
    ++read;
    MOZ_RELEASE_ASSERT(er.RemainingBytes() == sizeof(test));
    const auto value = er.ReadObject<decltype(test)>();
    MOZ_RELEASE_ASSERT(value == test);
    MOZ_RELEASE_ASSERT(er.RemainingBytes() == 0);
  });
  MOZ_RELEASE_ASSERT(read == 1);

  // Read the int directly from the ProfileChunkedBuffer, with block index.
  read = 0;
  blockIndex = nullptr;
  cb.ReadEach(
      [&](ProfileBufferEntryReader& er, ProfileBufferBlockIndex aBlockIndex) {
        ++read;
        MOZ_RELEASE_ASSERT(!!aBlockIndex);
        MOZ_RELEASE_ASSERT(!blockIndex);
        blockIndex = aBlockIndex;
        MOZ_RELEASE_ASSERT(er.RemainingBytes() == sizeof(test));
        const auto value = er.ReadObject<decltype(test)>();
        MOZ_RELEASE_ASSERT(value == test);
        MOZ_RELEASE_ASSERT(er.RemainingBytes() == 0);
      });
  MOZ_RELEASE_ASSERT(read == 1);
  MOZ_RELEASE_ASSERT(!!blockIndex);
  MOZ_RELEASE_ASSERT(blockIndex != nullptr);

  // Read the int from its block index.
  read = 0;
  result = 0;
  result = cb.ReadAt(blockIndex, [&](Maybe<ProfileBufferEntryReader>&& er) {
    ++read;
    MOZ_RELEASE_ASSERT(er.isSome());
    MOZ_RELEASE_ASSERT(er->CurrentBlockIndex() == blockIndex);
    MOZ_RELEASE_ASSERT(!er->NextBlockIndex());
    MOZ_RELEASE_ASSERT(er->RemainingBytes() == sizeof(test));
    const auto value = er->ReadObject<decltype(test)>();
    MOZ_RELEASE_ASSERT(value == test);
    MOZ_RELEASE_ASSERT(er->RemainingBytes() == 0);
    return 6;
  });
  MOZ_RELEASE_ASSERT(result == 6);
  MOZ_RELEASE_ASSERT(read == 1);

  MOZ_RELEASE_ASSERT(!cb.IsIndexInCurrentChunk(ProfileBufferIndex{}));
  MOZ_RELEASE_ASSERT(
      cb.IsIndexInCurrentChunk(blockIndex.ConvertToProfileBufferIndex()));
  MOZ_RELEASE_ASSERT(cb.IsIndexInCurrentChunk(cb.GetState().mRangeEnd - 1));
  MOZ_RELEASE_ASSERT(!cb.IsIndexInCurrentChunk(cb.GetState().mRangeEnd));

  // No changes after reads.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cb, 1, 1 + ULEB128Size(sizeof(test)) + sizeof(test), 1, 0, 0);

  // Steal the underlying ProfileBufferChunks from the ProfileChunkedBuffer.
  chunks = cb.GetAllChunks();
  MOZ_RELEASE_ASSERT(!!chunks, "Expected at least one chunk");
  MOZ_RELEASE_ASSERT(!!chunks->GetNext(), "Expected two chunks");
  MOZ_RELEASE_ASSERT(!chunks->GetNext()->GetNext(), "Expected only two chunks");
  const ProfileChunkedBuffer::Length chunkActualSize = chunks->BufferBytes();
  MOZ_RELEASE_ASSERT(chunkActualSize >= chunkMinSize);
  MOZ_RELEASE_ASSERT(chunks->RangeStart() == 1);
  MOZ_RELEASE_ASSERT(chunks->OffsetFirstBlock() == 0);
  MOZ_RELEASE_ASSERT(chunks->OffsetPastLastBlock() == 1 + sizeof(test));

  // GetAllChunks() should have advanced the index one full chunk forward.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1 + chunkActualSize,
                                             1 + chunkActualSize, 1, 0, 0);

  // Nothing more to read from the now-empty ProfileChunkedBuffer.
  cb.ReadEach([](ProfileBufferEntryReader&) { MOZ_RELEASE_ASSERT(false); });
  cb.ReadEach([](ProfileBufferEntryReader&, ProfileBufferBlockIndex) {
    MOZ_RELEASE_ASSERT(false);
  });
  result = 0;
  result = cb.ReadAt(nullptr, [](Maybe<ProfileBufferEntryReader>&& er) {
    MOZ_RELEASE_ASSERT(er.isNothing());
    return 7;
  });
  MOZ_RELEASE_ASSERT(result == 7);

  // Read the int from the stolen chunks.
  read = 0;
  ProfileChunkedBuffer::ReadEach(
      chunks.get(), nullptr,
      [&](ProfileBufferEntryReader& er, ProfileBufferBlockIndex aBlockIndex) {
        ++read;
        MOZ_RELEASE_ASSERT(aBlockIndex == blockIndex);
        MOZ_RELEASE_ASSERT(er.RemainingBytes() == sizeof(test));
        const auto value = er.ReadObject<decltype(test)>();
        MOZ_RELEASE_ASSERT(value == test);
        MOZ_RELEASE_ASSERT(er.RemainingBytes() == 0);
      });
  MOZ_RELEASE_ASSERT(read == 1);

  // No changes after reads.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, 1 + chunkActualSize,
                                             1 + chunkActualSize, 1, 0, 0);

  // Write lots of numbers (by memcpy), which should trigger Chunk destructions.
  ProfileBufferBlockIndex firstBlockIndex;
  MOZ_RELEASE_ASSERT(!firstBlockIndex);
  ProfileBufferBlockIndex lastBlockIndex;
  MOZ_RELEASE_ASSERT(!lastBlockIndex);
  const size_t lots = 2 * bufferMaxSize / (1 + sizeof(int));
  for (size_t i = 1; i < lots; ++i) {
    ProfileBufferBlockIndex blockIndex = cb.PutFrom(&i, sizeof(i));
    MOZ_RELEASE_ASSERT(!!blockIndex);
    MOZ_RELEASE_ASSERT(blockIndex > firstBlockIndex);
    if (!firstBlockIndex) {
      firstBlockIndex = blockIndex;
    }
    MOZ_RELEASE_ASSERT(blockIndex > lastBlockIndex);
    lastBlockIndex = blockIndex;
  }

  ProfileChunkedBuffer::State stateAfterPuts = cb.GetState();
  ProfileBufferIndex startAfterPuts = stateAfterPuts.mRangeStart;
  MOZ_RELEASE_ASSERT(startAfterPuts > 1 + chunkActualSize);
  ProfileBufferIndex endAfterPuts = stateAfterPuts.mRangeEnd;
  MOZ_RELEASE_ASSERT(endAfterPuts > startAfterPuts);
  uint64_t pushedAfterPuts = stateAfterPuts.mPushedBlockCount;
  MOZ_RELEASE_ASSERT(pushedAfterPuts > 0);
  uint64_t clearedAfterPuts = stateAfterPuts.mClearedBlockCount;
  MOZ_RELEASE_ASSERT(clearedAfterPuts > 0);
  MOZ_RELEASE_ASSERT(stateAfterPuts.mFailedPutBytes == 0);
  MOZ_RELEASE_ASSERT(!cb.IsIndexInCurrentChunk(ProfileBufferIndex{}));
  MOZ_RELEASE_ASSERT(
      !cb.IsIndexInCurrentChunk(blockIndex.ConvertToProfileBufferIndex()));
  MOZ_RELEASE_ASSERT(
      !cb.IsIndexInCurrentChunk(firstBlockIndex.ConvertToProfileBufferIndex()));

  // Read extant numbers, which should at least follow each other.
  read = 0;
  size_t i = 0;
  cb.ReadEach(
      [&](ProfileBufferEntryReader& er, ProfileBufferBlockIndex aBlockIndex) {
        ++read;
        MOZ_RELEASE_ASSERT(!!aBlockIndex);
        MOZ_RELEASE_ASSERT(aBlockIndex > firstBlockIndex);
        MOZ_RELEASE_ASSERT(aBlockIndex <= lastBlockIndex);
        MOZ_RELEASE_ASSERT(er.RemainingBytes() == sizeof(size_t));
        const auto value = er.ReadObject<size_t>();
        if (i == 0) {
          i = value;
        } else {
          MOZ_RELEASE_ASSERT(value == ++i);
        }
        MOZ_RELEASE_ASSERT(er.RemainingBytes() == 0);
      });
  MOZ_RELEASE_ASSERT(read != 0);
  MOZ_RELEASE_ASSERT(read < lots);

  // Read first extant number.
  read = 0;
  i = 0;
  blockIndex = nullptr;
  success =
      cb.ReadAt(firstBlockIndex, [&](Maybe<ProfileBufferEntryReader>&& er) {
        MOZ_ASSERT(er.isSome());
        ++read;
        MOZ_RELEASE_ASSERT(er->CurrentBlockIndex() > firstBlockIndex);
        MOZ_RELEASE_ASSERT(!!er->NextBlockIndex());
        MOZ_RELEASE_ASSERT(er->NextBlockIndex() > firstBlockIndex);
        MOZ_RELEASE_ASSERT(er->NextBlockIndex() < lastBlockIndex);
        blockIndex = er->NextBlockIndex();
        MOZ_RELEASE_ASSERT(er->RemainingBytes() == sizeof(size_t));
        const auto value = er->ReadObject<size_t>();
        MOZ_RELEASE_ASSERT(i == 0);
        i = value;
        MOZ_RELEASE_ASSERT(er->RemainingBytes() == 0);
        return 7;
      });
  MOZ_RELEASE_ASSERT(success);
  MOZ_RELEASE_ASSERT(read == 1);
  // Read other extant numbers one by one.
  do {
    bool success =
        cb.ReadAt(blockIndex, [&](Maybe<ProfileBufferEntryReader>&& er) {
          MOZ_ASSERT(er.isSome());
          ++read;
          MOZ_RELEASE_ASSERT(er->CurrentBlockIndex() == blockIndex);
          MOZ_RELEASE_ASSERT(!er->NextBlockIndex() ||
                             er->NextBlockIndex() > blockIndex);
          MOZ_RELEASE_ASSERT(!er->NextBlockIndex() ||
                             er->NextBlockIndex() > firstBlockIndex);
          MOZ_RELEASE_ASSERT(!er->NextBlockIndex() ||
                             er->NextBlockIndex() <= lastBlockIndex);
          MOZ_RELEASE_ASSERT(er->NextBlockIndex()
                                 ? blockIndex < lastBlockIndex
                                 : blockIndex == lastBlockIndex,
                             "er->NextBlockIndex() should only be null when "
                             "blockIndex is at the last block");
          blockIndex = er->NextBlockIndex();
          MOZ_RELEASE_ASSERT(er->RemainingBytes() == sizeof(size_t));
          const auto value = er->ReadObject<size_t>();
          MOZ_RELEASE_ASSERT(value == ++i);
          MOZ_RELEASE_ASSERT(er->RemainingBytes() == 0);
          return true;
        });
    MOZ_RELEASE_ASSERT(success);
  } while (blockIndex);
  MOZ_RELEASE_ASSERT(read > 1);

  // No changes after reads.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cb, startAfterPuts, endAfterPuts, pushedAfterPuts, clearedAfterPuts, 0);

#  ifdef DEBUG
  // cb.Dump();
#  endif

  cb.Clear();

#  ifdef DEBUG
  // cb.Dump();
#  endif

  ProfileChunkedBuffer::State stateAfterClear = cb.GetState();
  ProfileBufferIndex startAfterClear = stateAfterClear.mRangeStart;
  MOZ_RELEASE_ASSERT(startAfterClear > startAfterPuts);
  ProfileBufferIndex endAfterClear = stateAfterClear.mRangeEnd;
  MOZ_RELEASE_ASSERT(endAfterClear == startAfterClear);
  MOZ_RELEASE_ASSERT(stateAfterClear.mPushedBlockCount == 0);
  MOZ_RELEASE_ASSERT(stateAfterClear.mClearedBlockCount == 0);
  MOZ_RELEASE_ASSERT(stateAfterClear.mFailedPutBytes == 0);
  MOZ_RELEASE_ASSERT(!cb.IsIndexInCurrentChunk(ProfileBufferIndex{}));
  MOZ_RELEASE_ASSERT(
      !cb.IsIndexInCurrentChunk(blockIndex.ConvertToProfileBufferIndex()));
  MOZ_RELEASE_ASSERT(!cb.IsIndexInCurrentChunk(stateAfterClear.mRangeEnd - 1));
  MOZ_RELEASE_ASSERT(!cb.IsIndexInCurrentChunk(stateAfterClear.mRangeEnd));

  // Start writer threads.
  constexpr int ThreadCount = 32;
  std::thread threads[ThreadCount];
  for (int threadNo = 0; threadNo < ThreadCount; ++threadNo) {
    threads[threadNo] = std::thread(
        [&](int aThreadNo) {
          ::SleepMilli(1);
          constexpr int pushCount = 1024;
          for (int push = 0; push < pushCount; ++push) {
            // Reserve as many bytes as the thread number (but at least enough
            // to store an int), and write an increasing int.
            const bool success =
                cb.Put(std::max(aThreadNo, int(sizeof(push))),
                       [&](Maybe<ProfileBufferEntryWriter>& aEW) {
                         if (!aEW) {
                           return false;
                         }
                         aEW->WriteObject(aThreadNo * 1000000 + push);
                         // Advance writer to the end.
                         for (size_t r = aEW->RemainingBytes(); r != 0; --r) {
                           aEW->WriteObject<char>('_');
                         }
                         return true;
                       });
            MOZ_RELEASE_ASSERT(success);
          }
        },
        threadNo);
  }

  // Wait for all writer threads to die.
  for (auto&& thread : threads) {
    thread.join();
  }

#  ifdef DEBUG
  // cb.Dump();
#  endif

  ProfileChunkedBuffer::State stateAfterMTPuts = cb.GetState();
  ProfileBufferIndex startAfterMTPuts = stateAfterMTPuts.mRangeStart;
  MOZ_RELEASE_ASSERT(startAfterMTPuts > startAfterClear);
  ProfileBufferIndex endAfterMTPuts = stateAfterMTPuts.mRangeEnd;
  MOZ_RELEASE_ASSERT(endAfterMTPuts > startAfterMTPuts);
  MOZ_RELEASE_ASSERT(stateAfterMTPuts.mPushedBlockCount > 0);
  MOZ_RELEASE_ASSERT(stateAfterMTPuts.mClearedBlockCount > 0);
  MOZ_RELEASE_ASSERT(stateAfterMTPuts.mFailedPutBytes == 0);

  // Reset to out-of-session.
  cb.ResetChunkManager();

  ProfileChunkedBuffer::State stateAfterReset = cb.GetState();
  ProfileBufferIndex startAfterReset = stateAfterReset.mRangeStart;
  MOZ_RELEASE_ASSERT(startAfterReset == endAfterMTPuts);
  ProfileBufferIndex endAfterReset = stateAfterReset.mRangeEnd;
  MOZ_RELEASE_ASSERT(endAfterReset == startAfterReset);
  MOZ_RELEASE_ASSERT(stateAfterReset.mPushedBlockCount == 0);
  MOZ_RELEASE_ASSERT(stateAfterReset.mClearedBlockCount == 0);
  MOZ_RELEASE_ASSERT(stateAfterReset.mFailedPutBytes == 0);

  success = cb.ReserveAndPut(
      []() {
        MOZ_RELEASE_ASSERT(false);
        return 1;
      },
      [](Maybe<ProfileBufferEntryWriter>& aEW) { return !!aEW; });
  MOZ_RELEASE_ASSERT(!success);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  success =
      cb.Put(1, [](Maybe<ProfileBufferEntryWriter>& aEW) { return !!aEW; });
  MOZ_RELEASE_ASSERT(!success);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  blockIndex = cb.PutFrom(&success, 1);
  MOZ_RELEASE_ASSERT(!blockIndex);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  blockIndex = cb.PutObjects(123, success, "hello");
  MOZ_RELEASE_ASSERT(!blockIndex);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  blockIndex = cb.PutObject(123);
  MOZ_RELEASE_ASSERT(!blockIndex);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  chunks = cb.GetAllChunks();
  MOZ_RELEASE_ASSERT(!chunks, "Expected no chunks when out-of-session");
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  cb.ReadEach([](ProfileBufferEntryReader&) { MOZ_RELEASE_ASSERT(false); });
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  success = cb.ReadAt(nullptr, [](Maybe<ProfileBufferEntryReader>&& er) {
    MOZ_RELEASE_ASSERT(er.isNothing());
    return true;
  });
  MOZ_RELEASE_ASSERT(success);
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cb, startAfterReset, endAfterReset,
                                             0, 0, 0);

  printf("TestChunkedBuffer done\n");
}

static void TestChunkedBufferSingle() {
  printf("TestChunkedBufferSingle...\n");

  constexpr ProfileChunkedBuffer::Length chunkMinSize = 128;

  // Create a ProfileChunkedBuffer that will own&use a
  // ProfileBufferChunkManagerSingle, which will give away one
  // ProfileBufferChunk that can contain 128 bytes.
  ProfileChunkedBuffer cbSingle(
      ProfileChunkedBuffer::ThreadSafety::WithoutMutex,
      MakeUnique<ProfileBufferChunkManagerSingle>(chunkMinSize));

  MOZ_RELEASE_ASSERT(cbSingle.BufferLength().isSome());
  const ProfileChunkedBuffer::Length bufferBytes = *cbSingle.BufferLength();
  MOZ_RELEASE_ASSERT(bufferBytes >= chunkMinSize);

  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cbSingle, 1, 1, 0, 0, 0);

  // We will write this many blocks to fill the chunk.
  constexpr size_t testBlocks = 4;
  const ProfileChunkedBuffer::Length blockBytes = bufferBytes / testBlocks;
  MOZ_RELEASE_ASSERT(ULEB128Size(blockBytes) == 1,
                     "This test assumes block sizes are small enough so that "
                     "their ULEB128-encoded size is 1 byte");
  const ProfileChunkedBuffer::Length entryBytes =
      blockBytes - ULEB128Size(blockBytes);

  // First buffer-filling test: Try to write a too-big entry at the end of the
  // chunk.

  // Write all but one block.
  for (size_t i = 0; i < testBlocks - 1; ++i) {
    cbSingle.Put(entryBytes, [&](Maybe<ProfileBufferEntryWriter>& aEW) {
      MOZ_RELEASE_ASSERT(aEW.isSome());
      while (aEW->RemainingBytes() > 0) {
        **aEW = '0' + i;
        ++(*aEW);
      }
    });
    VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
        cbSingle, 1, 1 + blockBytes * (i + 1), i + 1, 0, 0);
  }

  // Write the last block so that it's too big (by 1 byte) to fit in the chunk,
  // this should fail.
  const ProfileChunkedBuffer::Length remainingBytesForLastBlock =
      bufferBytes - blockBytes * (testBlocks - 1);
  MOZ_RELEASE_ASSERT(ULEB128Size(remainingBytesForLastBlock) == 1,
                     "This test assumes block sizes are small enough so that "
                     "their ULEB128-encoded size is 1 byte");
  const ProfileChunkedBuffer::Length entryToFitRemainingBytes =
      remainingBytesForLastBlock - ULEB128Size(remainingBytesForLastBlock);
  cbSingle.Put(entryToFitRemainingBytes + 1,
               [&](Maybe<ProfileBufferEntryWriter>& aEW) {
                 MOZ_RELEASE_ASSERT(aEW.isNothing());
               });
  // The buffer state should not have changed, apart from the failed bytes.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cbSingle, 1, 1 + blockBytes * (testBlocks - 1), testBlocks - 1, 0,
      remainingBytesForLastBlock + 1);

  size_t read = 0;
  cbSingle.ReadEach([&](ProfileBufferEntryReader& aER) {
    MOZ_RELEASE_ASSERT(aER.RemainingBytes() == entryBytes);
    while (aER.RemainingBytes() > 0) {
      MOZ_RELEASE_ASSERT(*aER == '0' + read);
      ++aER;
    }
    ++read;
  });
  MOZ_RELEASE_ASSERT(read == testBlocks - 1);

  // ~Interlude~ Test AppendContent:
  // Create another ProfileChunkedBuffer that will use a
  // ProfileBufferChunkManagerWithLocalLimit, which will give away
  // ProfileBufferChunks that can contain 128 bytes, using up to 1KB of memory
  // (including usable 128 bytes and headers).
  constexpr size_t bufferMaxSize = 1024;
  ProfileBufferChunkManagerWithLocalLimit cmTarget(bufferMaxSize, chunkMinSize);
  ProfileChunkedBuffer cbTarget(ProfileChunkedBuffer::ThreadSafety::WithMutex,
                                cmTarget);

  // It should start empty.
  cbTarget.ReadEach(
      [](ProfileBufferEntryReader&) { MOZ_RELEASE_ASSERT(false); });
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cbTarget, 1, 1, 0, 0, 0);

  // Copy the contents from cbSingle to cbTarget.
  cbTarget.AppendContents(cbSingle);

  // And verify that we now have the same contents in cbTarget.
  read = 0;
  cbTarget.ReadEach([&](ProfileBufferEntryReader& aER) {
    MOZ_RELEASE_ASSERT(aER.RemainingBytes() == entryBytes);
    while (aER.RemainingBytes() > 0) {
      MOZ_RELEASE_ASSERT(*aER == '0' + read);
      ++aER;
    }
    ++read;
  });
  MOZ_RELEASE_ASSERT(read == testBlocks - 1);
  // The state should be the same as the source.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cbTarget, 1, 1 + blockBytes * (testBlocks - 1), testBlocks - 1, 0, 0);

#  ifdef DEBUG
  // cbSingle.Dump();
  // cbTarget.Dump();
#  endif

  // Because we failed to write a too-big chunk above, the chunk was marked
  // full, so that entries should be consistently rejected from now on.
  cbSingle.Put(1, [&](Maybe<ProfileBufferEntryWriter>& aEW) {
    MOZ_RELEASE_ASSERT(aEW.isNothing());
  });
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cbSingle, 1, 1 + blockBytes * ((testBlocks - 1)), testBlocks - 1, 0,
      remainingBytesForLastBlock + 1 + ULEB128Size(1u) + 1);

  // Clear the buffer before the next test.

  cbSingle.Clear();
  // Clear() should move the index to the next chunk range -- even if it's
  // really reusing the same chunk.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cbSingle, 1 + bufferBytes,
                                             1 + bufferBytes, 0, 0, 0);
  cbSingle.ReadEach(
      [&](ProfileBufferEntryReader& aER) { MOZ_RELEASE_ASSERT(false); });

  // Second buffer-filling test: Try to write a final entry that just fits at
  // the end of the chunk.

  // Write all but one block.
  for (size_t i = 0; i < testBlocks - 1; ++i) {
    cbSingle.Put(entryBytes, [&](Maybe<ProfileBufferEntryWriter>& aEW) {
      MOZ_RELEASE_ASSERT(aEW.isSome());
      while (aEW->RemainingBytes() > 0) {
        **aEW = 'a' + i;
        ++(*aEW);
      }
    });
    VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
        cbSingle, 1 + bufferBytes, 1 + bufferBytes + blockBytes * (i + 1),
        i + 1, 0, 0);
  }

  read = 0;
  cbSingle.ReadEach([&](ProfileBufferEntryReader& aER) {
    MOZ_RELEASE_ASSERT(aER.RemainingBytes() == entryBytes);
    while (aER.RemainingBytes() > 0) {
      MOZ_RELEASE_ASSERT(*aER == 'a' + read);
      ++aER;
    }
    ++read;
  });
  MOZ_RELEASE_ASSERT(read == testBlocks - 1);

  // Write the last block so that it fits exactly in the chunk.
  cbSingle.Put(entryToFitRemainingBytes,
               [&](Maybe<ProfileBufferEntryWriter>& aEW) {
                 MOZ_RELEASE_ASSERT(aEW.isSome());
                 while (aEW->RemainingBytes() > 0) {
                   **aEW = 'a' + (testBlocks - 1);
                   ++(*aEW);
                 }
               });
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cbSingle, 1 + bufferBytes, 1 + bufferBytes + blockBytes * testBlocks,
      testBlocks, 0, 0);

  read = 0;
  cbSingle.ReadEach([&](ProfileBufferEntryReader& aER) {
    MOZ_RELEASE_ASSERT(
        aER.RemainingBytes() ==
        ((read < testBlocks) ? entryBytes : entryToFitRemainingBytes));
    while (aER.RemainingBytes() > 0) {
      MOZ_RELEASE_ASSERT(*aER == 'a' + read);
      ++aER;
    }
    ++read;
  });
  MOZ_RELEASE_ASSERT(read == testBlocks);

  // Because the single chunk has been filled, it shouldn't be possible to write
  // more entries.
  cbSingle.Put(1, [&](Maybe<ProfileBufferEntryWriter>& aEW) {
    MOZ_RELEASE_ASSERT(aEW.isNothing());
  });
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cbSingle, 1 + bufferBytes, 1 + bufferBytes + blockBytes * testBlocks,
      testBlocks, 0, ULEB128Size(1u) + 1);

  cbSingle.Clear();
  // Clear() should move the index to the next chunk range -- even if it's
  // really reusing the same chunk.
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(cbSingle, 1 + bufferBytes * 2,
                                             1 + bufferBytes * 2, 0, 0, 0);
  cbSingle.ReadEach(
      [&](ProfileBufferEntryReader& aER) { MOZ_RELEASE_ASSERT(false); });

  // Clear() recycles the released chunk, so we should be able to record new
  // entries.
  cbSingle.Put(entryBytes, [&](Maybe<ProfileBufferEntryWriter>& aEW) {
    MOZ_RELEASE_ASSERT(aEW.isSome());
    while (aEW->RemainingBytes() > 0) {
      **aEW = 'x';
      ++(*aEW);
    }
  });
  VERIFY_PCB_START_END_PUSHED_CLEARED_FAILED(
      cbSingle, 1 + bufferBytes * 2,
      1 + bufferBytes * 2 + ULEB128Size(entryBytes) + entryBytes, 1, 0, 0);
  read = 0;
  cbSingle.ReadEach([&](ProfileBufferEntryReader& aER) {
    MOZ_RELEASE_ASSERT(read == 0);
    MOZ_RELEASE_ASSERT(aER.RemainingBytes() == entryBytes);
    while (aER.RemainingBytes() > 0) {
      MOZ_RELEASE_ASSERT(*aER == 'x');
      ++aER;
    }
    ++read;
  });
  MOZ_RELEASE_ASSERT(read == 1);

  printf("TestChunkedBufferSingle done\n");
}

static void TestModuloBuffer(ModuloBuffer<>& mb, uint32_t MBSize) {
  using MB = ModuloBuffer<>;

  MOZ_RELEASE_ASSERT(mb.BufferLength().Value() == MBSize);

  // Iterator comparisons.
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) == mb.ReaderAt(2));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) != mb.ReaderAt(3));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) < mb.ReaderAt(3));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) <= mb.ReaderAt(2));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) <= mb.ReaderAt(3));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(3) > mb.ReaderAt(2));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) >= mb.ReaderAt(2));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(3) >= mb.ReaderAt(2));

  // Iterators indices don't wrap around (even though they may be pointing at
  // the same location).
  MOZ_RELEASE_ASSERT(mb.ReaderAt(2) != mb.ReaderAt(MBSize + 2));
  MOZ_RELEASE_ASSERT(mb.ReaderAt(MBSize + 2) != mb.ReaderAt(2));

  // Dereference.
  static_assert(std::is_same<decltype(*mb.ReaderAt(0)), const MB::Byte&>::value,
                "Dereferencing from a reader should return const Byte*");
  static_assert(std::is_same<decltype(*mb.WriterAt(0)), MB::Byte&>::value,
                "Dereferencing from a writer should return Byte*");
  // Contiguous between 0 and MBSize-1.
  MOZ_RELEASE_ASSERT(&*mb.ReaderAt(MBSize - 1) ==
                     &*mb.ReaderAt(0) + (MBSize - 1));
  // Wraps around.
  MOZ_RELEASE_ASSERT(&*mb.ReaderAt(MBSize) == &*mb.ReaderAt(0));
  MOZ_RELEASE_ASSERT(&*mb.ReaderAt(MBSize + MBSize - 1) ==
                     &*mb.ReaderAt(MBSize - 1));
  MOZ_RELEASE_ASSERT(&*mb.ReaderAt(MBSize + MBSize) == &*mb.ReaderAt(0));
  // Power of 2 modulo wrapping.
  MOZ_RELEASE_ASSERT(&*mb.ReaderAt(uint32_t(-1)) == &*mb.ReaderAt(MBSize - 1));
  MOZ_RELEASE_ASSERT(&*mb.ReaderAt(static_cast<MB::Index>(-1)) ==
                     &*mb.ReaderAt(MBSize - 1));

  // Arithmetic.
  MB::Reader arit = mb.ReaderAt(0);
  MOZ_RELEASE_ASSERT(++arit == mb.ReaderAt(1));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(1));

  MOZ_RELEASE_ASSERT(--arit == mb.ReaderAt(0));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(0));

  MOZ_RELEASE_ASSERT(arit++ == mb.ReaderAt(0));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(1));

  MOZ_RELEASE_ASSERT(arit-- == mb.ReaderAt(1));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(0));

  MOZ_RELEASE_ASSERT(arit + 3 == mb.ReaderAt(3));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(0));

  MOZ_RELEASE_ASSERT(4 + arit == mb.ReaderAt(4));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(0));

  // (Can't have assignments inside asserts, hence the split.)
  const bool checkPlusEq = ((arit += 3) == mb.ReaderAt(3));
  MOZ_RELEASE_ASSERT(checkPlusEq);
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(3));

  MOZ_RELEASE_ASSERT((arit - 2) == mb.ReaderAt(1));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(3));

  const bool checkMinusEq = ((arit -= 2) == mb.ReaderAt(1));
  MOZ_RELEASE_ASSERT(checkMinusEq);
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(1));

  // Random access.
  MOZ_RELEASE_ASSERT(&arit[3] == &*(arit + 3));
  MOZ_RELEASE_ASSERT(arit == mb.ReaderAt(1));

  // Iterator difference.
  MOZ_RELEASE_ASSERT(mb.ReaderAt(3) - mb.ReaderAt(1) == 2);
  MOZ_RELEASE_ASSERT(mb.ReaderAt(1) - mb.ReaderAt(3) == MB::Index(-2));

  // Only testing Writer, as Reader is just a subset with no code differences.
  MB::Writer it = mb.WriterAt(0);
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == 0);

  // Write two characters at the start.
  it.WriteObject('x');
  it.WriteObject('y');

  // Backtrack to read them.
  it -= 2;
  // PeekObject should read without moving.
  MOZ_RELEASE_ASSERT(it.PeekObject<char>() == 'x');
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == 0);
  // ReadObject should read and move past the character.
  MOZ_RELEASE_ASSERT(it.ReadObject<char>() == 'x');
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == 1);
  MOZ_RELEASE_ASSERT(it.PeekObject<char>() == 'y');
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == 1);
  MOZ_RELEASE_ASSERT(it.ReadObject<char>() == 'y');
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == 2);

  // Checking that a reader can be created from a writer.
  MB::Reader it2(it);
  MOZ_RELEASE_ASSERT(it2.CurrentIndex() == 2);
  // Or assigned.
  it2 = it;
  MOZ_RELEASE_ASSERT(it2.CurrentIndex() == 2);

  // Iterator traits.
  static_assert(std::is_same<std::iterator_traits<MB::Reader>::difference_type,
                             MB::Index>::value,
                "ModuloBuffer::Reader::difference_type should be Index");
  static_assert(std::is_same<std::iterator_traits<MB::Reader>::value_type,
                             MB::Byte>::value,
                "ModuloBuffer::Reader::value_type should be Byte");
  static_assert(std::is_same<std::iterator_traits<MB::Reader>::pointer,
                             const MB::Byte*>::value,
                "ModuloBuffer::Reader::pointer should be const Byte*");
  static_assert(std::is_same<std::iterator_traits<MB::Reader>::reference,
                             const MB::Byte&>::value,
                "ModuloBuffer::Reader::reference should be const Byte&");
  static_assert(std::is_base_of<
                    std::input_iterator_tag,
                    std::iterator_traits<MB::Reader>::iterator_category>::value,
                "ModuloBuffer::Reader::iterator_category should be derived "
                "from input_iterator_tag");
  static_assert(std::is_base_of<
                    std::forward_iterator_tag,
                    std::iterator_traits<MB::Reader>::iterator_category>::value,
                "ModuloBuffer::Reader::iterator_category should be derived "
                "from forward_iterator_tag");
  static_assert(std::is_base_of<
                    std::bidirectional_iterator_tag,
                    std::iterator_traits<MB::Reader>::iterator_category>::value,
                "ModuloBuffer::Reader::iterator_category should be derived "
                "from bidirectional_iterator_tag");
  static_assert(
      std::is_same<std::iterator_traits<MB::Reader>::iterator_category,
                   std::random_access_iterator_tag>::value,
      "ModuloBuffer::Reader::iterator_category should be "
      "random_access_iterator_tag");

  // Use as input iterator by std::string constructor (which is only considered
  // with proper input iterators.)
  std::string s(mb.ReaderAt(0), mb.ReaderAt(2));
  MOZ_RELEASE_ASSERT(s == "xy");

  // Write 4-byte number at index 2.
  it.WriteObject(int32_t(123));
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == 6);
  // And another, which should now wrap around (but index continues on.)
  it.WriteObject(int32_t(456));
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == MBSize + 2);
  // Even though index==MBSize+2, we can read the object we wrote at 2.
  MOZ_RELEASE_ASSERT(it.ReadObject<int32_t>() == 123);
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == MBSize + 6);
  // And similarly, index MBSize+6 points at the same location as index 6.
  MOZ_RELEASE_ASSERT(it.ReadObject<int32_t>() == 456);
  MOZ_RELEASE_ASSERT(it.CurrentIndex() == MBSize + MBSize + 2);
}

void TestModuloBuffer() {
  printf("TestModuloBuffer...\n");

  // Testing ModuloBuffer with default template arguments.
  using MB = ModuloBuffer<>;

  // Only 8-byte buffers, to easily test wrap-around.
  constexpr uint32_t MBSize = 8;

  // MB with self-allocated heap buffer.
  MB mbByLength(MakePowerOfTwo32<MBSize>());
  TestModuloBuffer(mbByLength, MBSize);

  // MB taking ownership of a provided UniquePtr to a buffer.
  auto uniqueBuffer = MakeUnique<uint8_t[]>(MBSize);
  MB mbByUniquePtr(MakeUnique<uint8_t[]>(MBSize), MakePowerOfTwo32<MBSize>());
  TestModuloBuffer(mbByUniquePtr, MBSize);

  // MB using part of a buffer on the stack. The buffer is three times the
  // required size: The middle third is where ModuloBuffer will work, the first
  // and last thirds are only used to later verify that ModuloBuffer didn't go
  // out of its bounds.
  uint8_t buffer[MBSize * 3];
  // Pre-fill the buffer with a known pattern, so we can later see what changed.
  for (size_t i = 0; i < MBSize * 3; ++i) {
    buffer[i] = uint8_t('A' + i);
  }
  MB mbByBuffer(&buffer[MBSize], MakePowerOfTwo32<MBSize>());
  TestModuloBuffer(mbByBuffer, MBSize);

  // Check that only the provided stack-based sub-buffer was modified.
  uint32_t changed = 0;
  for (size_t i = MBSize; i < MBSize * 2; ++i) {
    changed += (buffer[i] == uint8_t('A' + i)) ? 0 : 1;
  }
  // Expect at least 75% changes.
  MOZ_RELEASE_ASSERT(changed >= MBSize * 6 / 8);

  // Everything around the sub-buffer should be unchanged.
  for (size_t i = 0; i < MBSize; ++i) {
    MOZ_RELEASE_ASSERT(buffer[i] == uint8_t('A' + i));
  }
  for (size_t i = MBSize * 2; i < MBSize * 3; ++i) {
    MOZ_RELEASE_ASSERT(buffer[i] == uint8_t('A' + i));
  }

  // Check that move-construction is allowed. This verifies that we do not
  // crash from a double free, when `mbByBuffer` and `mbByStolenBuffer` are both
  // destroyed at the end of this function.
  MB mbByStolenBuffer = std::move(mbByBuffer);
  TestModuloBuffer(mbByStolenBuffer, MBSize);

  // Check that only the provided stack-based sub-buffer was modified.
  changed = 0;
  for (size_t i = MBSize; i < MBSize * 2; ++i) {
    changed += (buffer[i] == uint8_t('A' + i)) ? 0 : 1;
  }
  // Expect at least 75% changes.
  MOZ_RELEASE_ASSERT(changed >= MBSize * 6 / 8);

  // Everything around the sub-buffer should be unchanged.
  for (size_t i = 0; i < MBSize; ++i) {
    MOZ_RELEASE_ASSERT(buffer[i] == uint8_t('A' + i));
  }
  for (size_t i = MBSize * 2; i < MBSize * 3; ++i) {
    MOZ_RELEASE_ASSERT(buffer[i] == uint8_t('A' + i));
  }

  // This test function does a `ReadInto` as directed, and checks that the
  // result is the same as if the copy had been done manually byte-by-byte.
  // `TestReadInto(3, 7, 2)` copies from index 3 to index 7, 2 bytes long.
  // Return the output string (from `ReadInto`) for external checks.
  auto TestReadInto = [](MB::Index aReadFrom, MB::Index aWriteTo,
                         MB::Length aBytes) {
    constexpr uint32_t TRISize = 16;

    // Prepare an input buffer, all different elements.
    uint8_t input[TRISize + 1] = "ABCDEFGHIJKLMNOP";
    const MB mbInput(input, MakePowerOfTwo32<TRISize>());

    // Prepare an output buffer, different from input.
    uint8_t output[TRISize + 1] = "abcdefghijklmnop";
    MB mbOutput(output, MakePowerOfTwo32<TRISize>());

    // Run ReadInto.
    auto writer = mbOutput.WriterAt(aWriteTo);
    mbInput.ReaderAt(aReadFrom).ReadInto(writer, aBytes);

    // Do the same operation manually.
    uint8_t outputCheck[TRISize + 1] = "abcdefghijklmnop";
    MB mbOutputCheck(outputCheck, MakePowerOfTwo32<TRISize>());
    auto readerCheck = mbInput.ReaderAt(aReadFrom);
    auto writerCheck = mbOutputCheck.WriterAt(aWriteTo);
    for (MB::Length i = 0; i < aBytes; ++i) {
      *writerCheck++ = *readerCheck++;
    }

    // Compare the two outputs.
    for (uint32_t i = 0; i < TRISize; ++i) {
#  ifdef TEST_MODULOBUFFER_FAILURE_DEBUG
      // Only used when debugging failures.
      if (output[i] != outputCheck[i]) {
        printf(
            "*** from=%u to=%u bytes=%u i=%u\ninput:  '%s'\noutput: "
            "'%s'\ncheck:  '%s'\n",
            unsigned(aReadFrom), unsigned(aWriteTo), unsigned(aBytes),
            unsigned(i), input, output, outputCheck);
      }
#  endif
      MOZ_RELEASE_ASSERT(output[i] == outputCheck[i]);
    }

#  ifdef TEST_MODULOBUFFER_HELPER
    // Only used when adding more tests.
    printf("*** from=%u to=%u bytes=%u output: %s\n", unsigned(aReadFrom),
           unsigned(aWriteTo), unsigned(aBytes), output);
#  endif

    return std::string(reinterpret_cast<const char*>(output));
  };

  // A few manual checks:
  constexpr uint32_t TRISize = 16;
  MOZ_RELEASE_ASSERT(TestReadInto(0, 0, 0) == "abcdefghijklmnop");
  MOZ_RELEASE_ASSERT(TestReadInto(0, 0, TRISize) == "ABCDEFGHIJKLMNOP");
  MOZ_RELEASE_ASSERT(TestReadInto(0, 5, TRISize) == "LMNOPABCDEFGHIJK");
  MOZ_RELEASE_ASSERT(TestReadInto(5, 0, TRISize) == "FGHIJKLMNOPABCDE");

  // Test everything! (16^3 = 4096, not too much.)
  for (MB::Index r = 0; r < TRISize; ++r) {
    for (MB::Index w = 0; w < TRISize; ++w) {
      for (MB::Length len = 0; len < TRISize; ++len) {
        TestReadInto(r, w, len);
      }
    }
  }

  printf("TestModuloBuffer done\n");
}

void TestLiteralEmptyStringView() {
  printf("TestLiteralEmptyStringView...\n");

  static_assert(mozilla::LiteralEmptyStringView<char>() ==
                std::string_view(""));
  static_assert(!!mozilla::LiteralEmptyStringView<char>().data());
  static_assert(mozilla::LiteralEmptyStringView<char>().length() == 0);

  static_assert(mozilla::LiteralEmptyStringView<char16_t>() ==
                std::basic_string_view<char16_t>(u""));
  static_assert(!!mozilla::LiteralEmptyStringView<char16_t>().data());
  static_assert(mozilla::LiteralEmptyStringView<char16_t>().length() == 0);

  printf("TestLiteralEmptyStringView done\n");
}

template <typename CHAR>
void TestProfilerStringView() {
  if constexpr (std::is_same_v<CHAR, char>) {
    printf("TestProfilerStringView<char>...\n");
  } else if constexpr (std::is_same_v<CHAR, char16_t>) {
    printf("TestProfilerStringView<char16_t>...\n");
  } else {
    MOZ_RELEASE_ASSERT(false,
                       "TestProfilerStringView only handles char and char16_t");
  }

  // Used to verify implicit constructions, as this will normally be used in
  // function parameters.
  auto BSV = [](mozilla::ProfilerStringView<CHAR>&& aBSV) {
    return std::move(aBSV);
  };

  // These look like string literals, as expected by some string constructors.
  const CHAR empty[0 + 1] = {CHAR('\0')};
  const CHAR hi[2 + 1] = {
      CHAR('h'),
      CHAR('i'),
      CHAR('\0'),
  };

  // Literal empty string.
  MOZ_RELEASE_ASSERT(BSV(empty).Length() == 0);
  MOZ_RELEASE_ASSERT(BSV(empty).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(BSV(empty).IsLiteral());
  MOZ_RELEASE_ASSERT(!BSV(empty).IsReference());

  // Literal non-empty string.
  MOZ_RELEASE_ASSERT(BSV(hi).Length() == 2);
  MOZ_RELEASE_ASSERT(BSV(hi).AsSpan().Elements());
  MOZ_RELEASE_ASSERT(BSV(hi).AsSpan().Elements()[0] == CHAR('h'));
  MOZ_RELEASE_ASSERT(BSV(hi).AsSpan().Elements()[1] == CHAR('i'));
  MOZ_RELEASE_ASSERT(BSV(hi).IsLiteral());
  MOZ_RELEASE_ASSERT(!BSV(hi).IsReference());

  // std::string_view to a literal empty string.
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>(empty)).Length() == 0);
  MOZ_RELEASE_ASSERT(
      BSV(std::basic_string_view<CHAR>(empty)).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(!BSV(std::basic_string_view<CHAR>(empty)).IsLiteral());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>(empty)).IsReference());

  // std::string_view to a literal non-empty string.
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>(hi)).Length() == 2);
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>(hi)).AsSpan().Elements());
  MOZ_RELEASE_ASSERT(
      BSV(std::basic_string_view<CHAR>(hi)).AsSpan().Elements()[0] ==
      CHAR('h'));
  MOZ_RELEASE_ASSERT(
      BSV(std::basic_string_view<CHAR>(hi)).AsSpan().Elements()[1] ==
      CHAR('i'));
  MOZ_RELEASE_ASSERT(!BSV(std::basic_string_view<CHAR>(hi)).IsLiteral());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>(hi)).IsReference());

  // Default std::string_view points at nullptr, ProfilerStringView converts it
  // to the literal empty string.
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>()).Length() == 0);
  MOZ_RELEASE_ASSERT(!std::basic_string_view<CHAR>().data());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>()).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string_view<CHAR>()).IsLiteral());
  MOZ_RELEASE_ASSERT(!BSV(std::basic_string_view<CHAR>()).IsReference());

  // std::string to a literal empty string.
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(empty)).Length() == 0);
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(empty)).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(!BSV(std::basic_string<CHAR>(empty)).IsLiteral());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(empty)).IsReference());

  // std::string to a literal non-empty string.
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(hi)).Length() == 2);
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(hi)).AsSpan().Elements());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(hi)).AsSpan().Elements()[0] ==
                     CHAR('h'));
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(hi)).AsSpan().Elements()[1] ==
                     CHAR('i'));
  MOZ_RELEASE_ASSERT(!BSV(std::basic_string<CHAR>(hi)).IsLiteral());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>(hi)).IsReference());

  // Default std::string contains an empty null-terminated string.
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>()).Length() == 0);
  MOZ_RELEASE_ASSERT(std::basic_string<CHAR>().data());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>()).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(!BSV(std::basic_string<CHAR>()).IsLiteral());
  MOZ_RELEASE_ASSERT(BSV(std::basic_string<CHAR>()).IsReference());

  // Class that quacks like nsTString (with Data(), Length(), IsLiteral()), to
  // check that ProfilerStringView can read from them.
  class FakeNsTString {
   public:
    FakeNsTString(const CHAR* aData, size_t aLength, bool aIsLiteral)
        : mData(aData), mLength(aLength), mIsLiteral(aIsLiteral) {}

    const CHAR* Data() const { return mData; }
    size_t Length() const { return mLength; }
    bool IsLiteral() const { return mIsLiteral; }

   private:
    const CHAR* mData;
    size_t mLength;
    bool mIsLiteral;
  };

  // FakeNsTString to nullptr.
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(nullptr, 0, true)).Length() == 0);
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(nullptr, 0, true)).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(nullptr, 0, true)).IsLiteral());
  MOZ_RELEASE_ASSERT(!BSV(FakeNsTString(nullptr, 0, true)).IsReference());

  // FakeNsTString to a literal empty string.
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(empty, 0, true)).Length() == 0);
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(empty, 0, true)).AsSpan().IsEmpty());
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(empty, 0, true)).IsLiteral());
  MOZ_RELEASE_ASSERT(!BSV(FakeNsTString(empty, 0, true)).IsReference());

  // FakeNsTString to a literal non-empty string.
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, true)).Length() == 2);
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, true)).AsSpan().Elements());
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, true)).AsSpan().Elements()[0] ==
                     CHAR('h'));
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, true)).AsSpan().Elements()[1] ==
                     CHAR('i'));
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, true)).IsLiteral());
  MOZ_RELEASE_ASSERT(!BSV(FakeNsTString(hi, 2, true)).IsReference());

  // FakeNsTString to a non-literal non-empty string.
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, false)).Length() == 2);
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, false)).AsSpan().Elements());
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, false)).AsSpan().Elements()[0] ==
                     CHAR('h'));
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, false)).AsSpan().Elements()[1] ==
                     CHAR('i'));
  MOZ_RELEASE_ASSERT(!BSV(FakeNsTString(hi, 2, false)).IsLiteral());
  MOZ_RELEASE_ASSERT(BSV(FakeNsTString(hi, 2, false)).IsReference());

  // Serialization and deserialization (with ownership).
  constexpr size_t bufferMaxSize = 1024;
  constexpr ProfileChunkedBuffer::Length chunkMinSize = 128;
  ProfileBufferChunkManagerWithLocalLimit cm(bufferMaxSize, chunkMinSize);
  ProfileChunkedBuffer cb(ProfileChunkedBuffer::ThreadSafety::WithMutex, cm);

  // Literal string, serialized as raw pointer.
  MOZ_RELEASE_ASSERT(cb.PutObject(BSV(hi)));
  {
    unsigned read = 0;
    ProfilerStringView<CHAR> outerBSV;
    cb.ReadEach([&](ProfileBufferEntryReader& aER) {
      ++read;
      auto bsv = aER.ReadObject<ProfilerStringView<CHAR>>();
      MOZ_RELEASE_ASSERT(bsv.Length() == 2);
      MOZ_RELEASE_ASSERT(bsv.AsSpan().Elements());
      MOZ_RELEASE_ASSERT(bsv.AsSpan().Elements()[0] == CHAR('h'));
      MOZ_RELEASE_ASSERT(bsv.AsSpan().Elements()[1] == CHAR('i'));
      MOZ_RELEASE_ASSERT(bsv.IsLiteral());
      MOZ_RELEASE_ASSERT(!bsv.IsReference());
      outerBSV = std::move(bsv);
    });
    MOZ_RELEASE_ASSERT(read == 1);
    MOZ_RELEASE_ASSERT(outerBSV.Length() == 2);
    MOZ_RELEASE_ASSERT(outerBSV.AsSpan().Elements());
    MOZ_RELEASE_ASSERT(outerBSV.AsSpan().Elements()[0] == CHAR('h'));
    MOZ_RELEASE_ASSERT(outerBSV.AsSpan().Elements()[1] == CHAR('i'));
    MOZ_RELEASE_ASSERT(outerBSV.IsLiteral());
    MOZ_RELEASE_ASSERT(!outerBSV.IsReference());
  }

  MOZ_RELEASE_ASSERT(cb.GetState().mRangeStart == 1u);

  cb.Clear();

  // Non-literal string, content is serialized.

  // We'll try to write 4 strings, such that the 4th one will cross into the
  // next chunk.
  unsigned guessedChunkBytes = unsigned(cb.GetState().mRangeStart) - 1u;
  static constexpr unsigned stringCount = 4u;
  const unsigned stringSize =
      guessedChunkBytes / stringCount / sizeof(CHAR) + 3u;

  std::basic_string<CHAR> longString;
  longString.reserve(stringSize);
  for (unsigned i = 0; i < stringSize; ++i) {
    longString += CHAR('0' + i);
  }

  for (unsigned i = 0; i < stringCount; ++i) {
    MOZ_RELEASE_ASSERT(cb.PutObject(BSV(longString)));
  }

  {
    unsigned read = 0;
    ProfilerStringView<CHAR> outerBSV;
    cb.ReadEach([&](ProfileBufferEntryReader& aER) {
      ++read;
      {
        auto bsv = aER.ReadObject<ProfilerStringView<CHAR>>();
        MOZ_RELEASE_ASSERT(bsv.Length() == stringSize);
        MOZ_RELEASE_ASSERT(bsv.AsSpan().Elements());
        for (unsigned i = 0; i < stringSize; ++i) {
          MOZ_RELEASE_ASSERT(bsv.AsSpan().Elements()[i] == CHAR('0' + i));
          longString += '0' + i;
        }
        MOZ_RELEASE_ASSERT(!bsv.IsLiteral());
        // The first 3 should be references (because they fit in one chunk, so
        // they can be referenced directly), which the 4th one have to be copied
        // out of two chunks and stitched back together.
        MOZ_RELEASE_ASSERT(bsv.IsReference() == (read != 4));

        // Test move of ownership.
        outerBSV = std::move(bsv);
        // After a move, references stay complete, while a non-reference had a
        // buffer that has been moved out.
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        MOZ_RELEASE_ASSERT(bsv.Length() == ((read != 4) ? stringSize : 0));
      }

      MOZ_RELEASE_ASSERT(outerBSV.Length() == stringSize);
      MOZ_RELEASE_ASSERT(outerBSV.AsSpan().Elements());
      for (unsigned i = 0; i < stringSize; ++i) {
        MOZ_RELEASE_ASSERT(outerBSV.AsSpan().Elements()[i] == CHAR('0' + i));
        longString += '0' + i;
      }
      MOZ_RELEASE_ASSERT(!outerBSV.IsLiteral());
      MOZ_RELEASE_ASSERT(outerBSV.IsReference() == (read != 4));
    });
    MOZ_RELEASE_ASSERT(read == 4);
  }

  if constexpr (std::is_same_v<CHAR, char>) {
    printf("TestProfilerStringView<char> done\n");
  } else if constexpr (std::is_same_v<CHAR, char16_t>) {
    printf("TestProfilerStringView<char16_t> done\n");
  }
}

void TestProfilerDependencies() {
  TestPowerOfTwoMask();
  TestPowerOfTwo();
  TestLEB128();
  TestJSONTimeOutput();
  TestChunk();
  TestChunkManagerSingle();
  TestChunkManagerWithLocalLimit();
  TestControlledChunkManagerUpdate();
  TestControlledChunkManagerWithLocalLimit();
  TestChunkedBuffer();
  TestChunkedBufferSingle();
  TestModuloBuffer();
  TestLiteralEmptyStringView();
  TestProfilerStringView<char>();
  TestProfilerStringView<char16_t>();
}

// Increase the depth, to a maximum (to avoid too-deep recursion).
static constexpr size_t NextDepth(size_t aDepth) {
  constexpr size_t MAX_DEPTH = 128;
  return (aDepth < MAX_DEPTH) ? (aDepth + 1) : aDepth;
}

Atomic<bool, Relaxed> sStopFibonacci;

// Compute fibonacci the hard way (recursively: `f(n)=f(n-1)+f(n-2)`), and
// prevent inlining.
// The template parameter makes each depth be a separate function, to better
// distinguish them in the profiler output.
template <size_t DEPTH = 0>
MOZ_NEVER_INLINE unsigned long long Fibonacci(unsigned long long n) {
  AUTO_BASE_PROFILER_LABEL_DYNAMIC_STRING("fib", OTHER, std::to_string(DEPTH));
  if (n == 0) {
    return 0;
  }
  if (n == 1) {
    return 1;
  }
  if (DEPTH < 5 && sStopFibonacci) {
    return 1'000'000'000;
  }
  TimeStamp start = TimeStamp::Now();
  static constexpr size_t MAX_MARKER_DEPTH = 10;
  unsigned long long f2 = Fibonacci<NextDepth(DEPTH)>(n - 2);
  if (DEPTH == 0) {
    BASE_PROFILER_MARKER_UNTYPED("Half-way through Fibonacci", OTHER);
  }
  unsigned long long f1 = Fibonacci<NextDepth(DEPTH)>(n - 1);
  if (DEPTH < MAX_MARKER_DEPTH) {
    BASE_PROFILER_MARKER_TEXT("fib", OTHER,
                              MarkerTiming::IntervalUntilNowFrom(start),
                              std::to_string(DEPTH));
  }
  return f2 + f1;
}

void TestProfiler() {
  printf("TestProfiler starting -- pid: %" PRIu64 ", tid: %" PRIu64 "\n",
         uint64_t(baseprofiler::profiler_current_process_id().ToNumber()),
         uint64_t(baseprofiler::profiler_current_thread_id().ToNumber()));
  // ::SleepMilli(10000);

  TestProfilerDependencies();

  {
    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_is_active());
    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_thread_is_being_profiled());
    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_thread_is_sleeping());

    const baseprofiler::BaseProfilerThreadId mainThreadId =
        mozilla::baseprofiler::profiler_current_thread_id();

    MOZ_RELEASE_ASSERT(mozilla::baseprofiler::profiler_main_thread_id() ==
                       mainThreadId);
    MOZ_RELEASE_ASSERT(mozilla::baseprofiler::profiler_is_main_thread());

    std::thread testThread([&]() {
      const baseprofiler::BaseProfilerThreadId testThreadId =
          mozilla::baseprofiler::profiler_current_thread_id();
      MOZ_RELEASE_ASSERT(testThreadId != mainThreadId);

      MOZ_RELEASE_ASSERT(mozilla::baseprofiler::profiler_main_thread_id() !=
                         testThreadId);
      MOZ_RELEASE_ASSERT(!mozilla::baseprofiler::profiler_is_main_thread());
    });
    testThread.join();

    printf("profiler_start()...\n");
    Vector<const char*> filters;
    // Profile all registered threads.
    MOZ_RELEASE_ASSERT(filters.append(""));
    const uint32_t features = baseprofiler::ProfilerFeature::StackWalk;
    baseprofiler::profiler_start(baseprofiler::BASE_PROFILER_DEFAULT_ENTRIES,
                                 BASE_PROFILER_DEFAULT_INTERVAL, features,
                                 filters.begin(), filters.length());

    MOZ_RELEASE_ASSERT(baseprofiler::profiler_is_active());
    MOZ_RELEASE_ASSERT(baseprofiler::profiler_thread_is_being_profiled());
    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_thread_is_sleeping());

    sStopFibonacci = false;

    std::thread threadFib([]() {
      AUTO_BASE_PROFILER_REGISTER_THREAD("fibonacci");
      SleepMilli(5);
      auto cause = baseprofiler::profiler_capture_backtrace();
      AUTO_BASE_PROFILER_MARKER_TEXT(
          "fibonacci", OTHER, MarkerStack::TakeBacktrace(std::move(cause)),
          "First leaf call");
      static const unsigned long long fibStart = 37;
      printf("Fibonacci(%llu)...\n", fibStart);
      AUTO_BASE_PROFILER_LABEL("Label around Fibonacci", OTHER);

      unsigned long long f = Fibonacci(fibStart);
      printf("Fibonacci(%llu) = %llu\n", fibStart, f);
    });

    std::thread threadCancelFib([]() {
      AUTO_BASE_PROFILER_REGISTER_THREAD("fibonacci canceller");
      SleepMilli(5);
      AUTO_BASE_PROFILER_MARKER_TEXT("fibonacci", OTHER, {}, "Canceller");
      static const int waitMaxSeconds = 10;
      for (int i = 0; i < waitMaxSeconds; ++i) {
        if (sStopFibonacci) {
          AUTO_BASE_PROFILER_LABEL_DYNAMIC_STRING("fibCancel", OTHER,
                                                  std::to_string(i));
          return;
        }
        AUTO_BASE_PROFILER_THREAD_SLEEP;
        SleepMilli(1000);
      }
      AUTO_BASE_PROFILER_LABEL_DYNAMIC_STRING("fibCancel", OTHER,
                                              "Cancelling!");
      sStopFibonacci = true;
    });

    {
      AUTO_BASE_PROFILER_MARKER_TEXT("main thread", OTHER, {},
                                     "joining fibonacci thread");
      AUTO_BASE_PROFILER_THREAD_SLEEP;
      threadFib.join();
    }

    {
      AUTO_BASE_PROFILER_MARKER_TEXT("main thread", OTHER, {},
                                     "joining fibonacci-canceller thread");
      sStopFibonacci = true;
      AUTO_BASE_PROFILER_THREAD_SLEEP;
      threadCancelFib.join();
    }

    // Just making sure all payloads know how to (de)serialize and stream.

    MOZ_RELEASE_ASSERT(
        baseprofiler::AddMarker("markers 2.0 without options (omitted)",
                                mozilla::baseprofiler::category::OTHER));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 without options (implicit brace-init)",
        mozilla::baseprofiler::category::OTHER, {}));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 without options (explicit init)",
        mozilla::baseprofiler::category::OTHER, MarkerOptions()));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 without options (explicit brace-init)",
        mozilla::baseprofiler::category::OTHER, MarkerOptions{}));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 with one option (implicit)",
        mozilla::baseprofiler::category::OTHER, MarkerInnerWindowId(123)));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 with one option (implicit brace-init)",
        mozilla::baseprofiler::category::OTHER, {MarkerInnerWindowId(123)}));

    MOZ_RELEASE_ASSERT(
        baseprofiler::AddMarker("markers 2.0 with one option (explicit init)",
                                mozilla::baseprofiler::category::OTHER,
                                MarkerOptions(MarkerInnerWindowId(123))));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 with one option (explicit brace-init)",
        mozilla::baseprofiler::category::OTHER,
        MarkerOptions{MarkerInnerWindowId(123)}));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 with two options (implicit brace-init)",
        mozilla::baseprofiler::category::OTHER,
        {MarkerInnerWindowId(123), MarkerStack::Capture()}));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 with two options (explicit init)",
        mozilla::baseprofiler::category::OTHER,
        MarkerOptions(MarkerInnerWindowId(123), MarkerStack::Capture())));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "markers 2.0 with two options (explicit brace-init)",
        mozilla::baseprofiler::category::OTHER,
        MarkerOptions{MarkerInnerWindowId(123), MarkerStack::Capture()}));

    MOZ_RELEASE_ASSERT(
        baseprofiler::AddMarker("default-templated markers 2.0 without options",
                                mozilla::baseprofiler::category::OTHER));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "default-templated markers 2.0 with option",
        mozilla::baseprofiler::category::OTHER, MarkerInnerWindowId(123)));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "explicitly-default-templated markers 2.0 without options",
        mozilla::baseprofiler::category::OTHER, {},
        ::mozilla::baseprofiler::markers::NoPayload{}));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "explicitly-default-templated markers 2.0 with option",
        mozilla::baseprofiler::category::OTHER, MarkerInnerWindowId(123),
        ::mozilla::baseprofiler::markers::NoPayload{}));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "tracing", mozilla::baseprofiler::category::OTHER, {},
        mozilla::baseprofiler::markers::Tracing{}, "category"));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "text", mozilla::baseprofiler::category::OTHER, {},
        mozilla::baseprofiler::markers::TextMarker{}, "text text"));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "media sample", mozilla::baseprofiler::category::OTHER, {},
        mozilla::baseprofiler::markers::MediaSampleMarker{}, 123, 456, 789));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "video falling behind", mozilla::baseprofiler::category::OTHER, {},
        mozilla::baseprofiler::markers::VideoFallingBehindMarker{}, 123, 456));

    MOZ_RELEASE_ASSERT(baseprofiler::AddMarker(
        "video sink render", mozilla::baseprofiler::category::OTHER, {},
        mozilla::baseprofiler::markers::VideoSinkRenderMarker{}, 123));

    printf("Sleep 1s...\n");
    {
      AUTO_BASE_PROFILER_THREAD_SLEEP;
      SleepMilli(1000);
    }

    printf("baseprofiler_pause()...\n");
    baseprofiler::profiler_pause();

    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_thread_is_being_profiled());

    Maybe<baseprofiler::ProfilerBufferInfo> info =
        baseprofiler::profiler_get_buffer_info();
    MOZ_RELEASE_ASSERT(info.isSome());
    printf("Profiler buffer range: %llu .. %llu (%llu bytes)\n",
           static_cast<unsigned long long>(info->mRangeStart),
           static_cast<unsigned long long>(info->mRangeEnd),
           // sizeof(ProfileBufferEntry) == 9
           (static_cast<unsigned long long>(info->mRangeEnd) -
            static_cast<unsigned long long>(info->mRangeStart)) *
               9);
    printf("Stats:         min(us) .. mean(us) .. max(us)  [count]\n");
    printf("- Intervals:   %7.1f .. %7.1f  .. %7.1f  [%u]\n",
           info->mIntervalsUs.min,
           info->mIntervalsUs.sum / info->mIntervalsUs.n,
           info->mIntervalsUs.max, info->mIntervalsUs.n);
    printf("- Overheads:   %7.1f .. %7.1f  .. %7.1f  [%u]\n",
           info->mOverheadsUs.min,
           info->mOverheadsUs.sum / info->mOverheadsUs.n,
           info->mOverheadsUs.max, info->mOverheadsUs.n);
    printf("  - Locking:   %7.1f .. %7.1f  .. %7.1f  [%u]\n",
           info->mLockingsUs.min, info->mLockingsUs.sum / info->mLockingsUs.n,
           info->mLockingsUs.max, info->mLockingsUs.n);
    printf("  - Clearning: %7.1f .. %7.1f  .. %7.1f  [%u]\n",
           info->mCleaningsUs.min,
           info->mCleaningsUs.sum / info->mCleaningsUs.n,
           info->mCleaningsUs.max, info->mCleaningsUs.n);
    printf("  - Counters:  %7.1f .. %7.1f  .. %7.1f  [%u]\n",
           info->mCountersUs.min, info->mCountersUs.sum / info->mCountersUs.n,
           info->mCountersUs.max, info->mCountersUs.n);
    printf("  - Threads:   %7.1f .. %7.1f  .. %7.1f  [%u]\n",
           info->mThreadsUs.min, info->mThreadsUs.sum / info->mThreadsUs.n,
           info->mThreadsUs.max, info->mThreadsUs.n);

    printf("baseprofiler_get_profile()...\n");
    UniquePtr<char[]> profile = baseprofiler::profiler_get_profile();

    // Use a string view over the profile contents, for easier testing.
    std::string_view profileSV = profile.get();

    constexpr const auto svnpos = std::string_view::npos;
    // TODO: Properly parse profile and check fields.
    // Check for some expected marker schema JSON output.
    MOZ_RELEASE_ASSERT(profileSV.find("\"markerSchema\":[") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"name\":\"Text\",") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"name\":\"tracing\",") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"name\":\"MediaSample\",") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"display\":[") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"marker-chart\"") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"marker-table\"") != svnpos);
    MOZ_RELEASE_ASSERT(profileSV.find("\"format\":\"string\"") != svnpos);
    // TODO: Add more checks for what's expected in the profile. Some of them
    // are done in gtest's.

    printf("baseprofiler_save_profile_to_file()...\n");
    baseprofiler::baseprofiler_save_profile_to_file(
        "TestProfiler_profile.json");

    printf("profiler_stop()...\n");
    baseprofiler::profiler_stop();

    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_is_active());
    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_thread_is_being_profiled());
    MOZ_RELEASE_ASSERT(!baseprofiler::profiler_thread_is_sleeping());

    printf("profiler_shutdown()...\n");
  }

  printf("TestProfiler done\n");
}

// Minimal string escaping, similar to how C++ stringliterals should be entered,
// to help update comparison strings in tests below.
void printEscaped(std::string_view aString) {
  for (const char c : aString) {
    switch (c) {
      case '\n':
        fprintf(stderr, "\\n\n");
        break;
      case '"':
        fprintf(stderr, "\\\"");
        break;
      case '\\':
        fprintf(stderr, "\\\\");
        break;
      default:
        if (c >= ' ' && c <= '~') {
          fprintf(stderr, "%c", c);
        } else {
          fprintf(stderr, "\\x%02x", unsigned(c));
        }
        break;
    }
  }
}

// Run aF(SpliceableChunkedJSONWriter&, UniqueJSONStrings&) from inside a JSON
// array, then output the string table, and compare the full output to
// aExpected.
template <typename F>
static void VerifyUniqueStringContents(
    F&& aF, std::string_view aExpectedData,
    std::string_view aExpectedUniqueStrings,
    mozilla::baseprofiler::UniqueJSONStrings* aUniqueStringsOrNull = nullptr) {
  mozilla::baseprofiler::SpliceableChunkedJSONWriter writer{
      FailureLatchInfallibleSource::Singleton()};

  MOZ_RELEASE_ASSERT(!writer.ChunkedWriteFunc().Fallible());
  MOZ_RELEASE_ASSERT(!writer.ChunkedWriteFunc().Failed());
  MOZ_RELEASE_ASSERT(!writer.ChunkedWriteFunc().GetFailure());
  MOZ_RELEASE_ASSERT(&writer.ChunkedWriteFunc().SourceFailureLatch() ==
                     &mozilla::FailureLatchInfallibleSource::Singleton());
  MOZ_RELEASE_ASSERT(
      &std::as_const(writer.ChunkedWriteFunc()).SourceFailureLatch() ==
      &mozilla::FailureLatchInfallibleSource::Singleton());

  MOZ_RELEASE_ASSERT(!writer.Fallible());
  MOZ_RELEASE_ASSERT(!writer.Failed());
  MOZ_RELEASE_ASSERT(!writer.GetFailure());
  MOZ_RELEASE_ASSERT(&writer.SourceFailureLatch() ==
                     &mozilla::FailureLatchInfallibleSource::Singleton());
  MOZ_RELEASE_ASSERT(&std::as_const(writer).SourceFailureLatch() ==
                     &mozilla::FailureLatchInfallibleSource::Singleton());

  // By default use a local UniqueJSONStrings, otherwise use the one provided.
  mozilla::baseprofiler::UniqueJSONStrings localUniqueStrings{
      FailureLatchInfallibleSource::Singleton()};
  MOZ_RELEASE_ASSERT(!localUniqueStrings.Fallible());
  MOZ_RELEASE_ASSERT(!localUniqueStrings.Failed());
  MOZ_RELEASE_ASSERT(!localUniqueStrings.GetFailure());
  MOZ_RELEASE_ASSERT(&localUniqueStrings.SourceFailureLatch() ==
                     &mozilla::FailureLatchInfallibleSource::Singleton());
  MOZ_RELEASE_ASSERT(&std::as_const(localUniqueStrings).SourceFailureLatch() ==
                     &mozilla::FailureLatchInfallibleSource::Singleton());

  mozilla::baseprofiler::UniqueJSONStrings& uniqueStrings =
      aUniqueStringsOrNull ? *aUniqueStringsOrNull : localUniqueStrings;
  MOZ_RELEASE_ASSERT(!uniqueStrings.Failed());
  MOZ_RELEASE_ASSERT(!uniqueStrings.GetFailure());

  writer.Start();
  {
    writer.StartArrayProperty("data");
    {
      std::forward<F>(aF)(writer, uniqueStrings);
    }
    writer.EndArray();

    writer.StartArrayProperty("stringTable");
    {
      uniqueStrings.SpliceStringTableElements(writer);
    }
    writer.EndArray();
  }
  writer.End();

  MOZ_RELEASE_ASSERT(!uniqueStrings.Failed());
  MOZ_RELEASE_ASSERT(!uniqueStrings.GetFailure());

  MOZ_RELEASE_ASSERT(!writer.ChunkedWriteFunc().Failed());
  MOZ_RELEASE_ASSERT(!writer.ChunkedWriteFunc().GetFailure());

  MOZ_RELEASE_ASSERT(!writer.Failed());
  MOZ_RELEASE_ASSERT(!writer.GetFailure());

  UniquePtr<char[]> jsonString = writer.ChunkedWriteFunc().CopyData();
  MOZ_RELEASE_ASSERT(jsonString);
  std::string_view jsonStringView(jsonString.get());
  const size_t length = writer.ChunkedWriteFunc().Length();
  MOZ_RELEASE_ASSERT(length == jsonStringView.length());
  std::string expected = "{\"data\":[";
  expected += aExpectedData;
  expected += "],\"stringTable\":[";
  expected += aExpectedUniqueStrings;
  expected += "]}";
  if (jsonStringView != expected) {
    fprintf(stderr,
            "Expected:\n"
            "------\n");
    printEscaped(expected);
    fprintf(stderr,
            "\n"
            "------\n"
            "Actual:\n"
            "------\n");
    printEscaped(jsonStringView);
    fprintf(stderr,
            "\n"
            "------\n");
  }
  MOZ_RELEASE_ASSERT(jsonStringView == expected);
}

void TestUniqueJSONStrings() {
  printf("TestUniqueJSONStrings...\n");

  using SCJW = mozilla::baseprofiler::SpliceableChunkedJSONWriter;
  using UJS = mozilla::baseprofiler::UniqueJSONStrings;

  // Empty everything.
  VerifyUniqueStringContents([](SCJW& aWriter, UJS& aUniqueStrings) {}, "", "");

  // Empty unique strings.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aWriter.StringElement("string");
      },
      R"("string")", "");

  // One unique string.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aUniqueStrings.WriteElement(aWriter, "string");
      },
      "0", R"("string")");

  // One unique string twice.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aUniqueStrings.WriteElement(aWriter, "string");
        aUniqueStrings.WriteElement(aWriter, "string");
      },
      "0,0", R"("string")");

  // Two single unique strings.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aUniqueStrings.WriteElement(aWriter, "string0");
        aUniqueStrings.WriteElement(aWriter, "string1");
      },
      "0,1", R"("string0","string1")");

  // Two unique strings with repetition.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aUniqueStrings.WriteElement(aWriter, "string0");
        aUniqueStrings.WriteElement(aWriter, "string1");
        aUniqueStrings.WriteElement(aWriter, "string0");
      },
      "0,1,0", R"("string0","string1")");

  // Mix some object properties, for coverage.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aUniqueStrings.WriteElement(aWriter, "string0");
        aWriter.StartObjectElement();
        {
          aUniqueStrings.WriteProperty(aWriter, "p0", "prop");
          aUniqueStrings.WriteProperty(aWriter, "p1", "string0");
          aUniqueStrings.WriteProperty(aWriter, "p2", "prop");
        }
        aWriter.EndObject();
        aUniqueStrings.WriteElement(aWriter, "string1");
        aUniqueStrings.WriteElement(aWriter, "string0");
        aUniqueStrings.WriteElement(aWriter, "prop");
      },
      R"(0,{"p0":1,"p1":0,"p2":1},2,0,1)", R"("string0","prop","string1")");

  // Unique string table with pre-existing data.
  {
    UJS ujs{FailureLatchInfallibleSource::Singleton()};
    {
      SCJW writer{FailureLatchInfallibleSource::Singleton()};
      ujs.WriteElement(writer, "external0");
      ujs.WriteElement(writer, "external1");
      ujs.WriteElement(writer, "external0");
    }
    VerifyUniqueStringContents(
        [](SCJW& aWriter, UJS& aUniqueStrings) {
          aUniqueStrings.WriteElement(aWriter, "string0");
          aUniqueStrings.WriteElement(aWriter, "string1");
          aUniqueStrings.WriteElement(aWriter, "string0");
        },
        "2,3,2", R"("external0","external1","string0","string1")", &ujs);
  }

  // Unique string table with pre-existing data from another table.
  {
    UJS ujs{FailureLatchInfallibleSource::Singleton()};
    {
      SCJW writer{FailureLatchInfallibleSource::Singleton()};
      ujs.WriteElement(writer, "external0");
      ujs.WriteElement(writer, "external1");
      ujs.WriteElement(writer, "external0");
    }
    UJS ujsCopy(FailureLatchInfallibleSource::Singleton(), ujs,
                mozilla::ProgressLogger{});
    VerifyUniqueStringContents(
        [](SCJW& aWriter, UJS& aUniqueStrings) {
          aUniqueStrings.WriteElement(aWriter, "string0");
          aUniqueStrings.WriteElement(aWriter, "string1");
          aUniqueStrings.WriteElement(aWriter, "string0");
        },
        "2,3,2", R"("external0","external1","string0","string1")", &ujs);
  }

  // Unique string table through SpliceableJSONWriter.
  VerifyUniqueStringContents(
      [](SCJW& aWriter, UJS& aUniqueStrings) {
        aWriter.SetUniqueStrings(aUniqueStrings);
        aWriter.UniqueStringElement("string0");
        aWriter.StartObjectElement();
        {
          aWriter.UniqueStringProperty("p0", "prop");
          aWriter.UniqueStringProperty("p1", "string0");
          aWriter.UniqueStringProperty("p2", "prop");
        }
        aWriter.EndObject();
        aWriter.UniqueStringElement("string1");
        aWriter.UniqueStringElement("string0");
        aWriter.UniqueStringElement("prop");
        aWriter.ResetUniqueStrings();
      },
      R"(0,{"p0":1,"p1":0,"p2":1},2,0,1)", R"("string0","prop","string1")");

  printf("TestUniqueJSONStrings done\n");
}

void StreamMarkers(const mozilla::ProfileChunkedBuffer& aBuffer,
                   mozilla::baseprofiler::SpliceableJSONWriter& aWriter) {
  aWriter.StartArrayProperty("data");
  {
    aBuffer.ReadEach([&](mozilla::ProfileBufferEntryReader& aEntryReader) {
      mozilla::ProfileBufferEntryKind entryKind =
          aEntryReader.ReadObject<mozilla::ProfileBufferEntryKind>();
      MOZ_RELEASE_ASSERT(entryKind == mozilla::ProfileBufferEntryKind::Marker);

      mozilla::base_profiler_markers_detail::DeserializeAfterKindAndStream(
          aEntryReader,
          [&](const mozilla::baseprofiler::BaseProfilerThreadId&) {
            return &aWriter;
          },
          [&](mozilla::ProfileChunkedBuffer&) {
            aWriter.StringElement("Real backtrace would be here");
          },
          [&](mozilla::base_profiler_markers_detail::Streaming::
                  DeserializerTag) {});
    });
  }
  aWriter.EndArray();
}

void PrintMarkers(const mozilla::ProfileChunkedBuffer& aBuffer) {
  mozilla::baseprofiler::SpliceableJSONWriter writer(
      mozilla::MakeUnique<mozilla::baseprofiler::OStreamJSONWriteFunc>(
          std::cout),
      FailureLatchInfallibleSource::Singleton());
  mozilla::baseprofiler::UniqueJSONStrings uniqueStrings{
      FailureLatchInfallibleSource::Singleton()};
  writer.SetUniqueStrings(uniqueStrings);
  writer.Start();
  {
    StreamMarkers(aBuffer, writer);

    writer.StartArrayProperty("stringTable");
    {
      uniqueStrings.SpliceStringTableElements(writer);
    }
    writer.EndArray();
  }
  writer.End();
  writer.ResetUniqueStrings();
}

static void SubTestMarkerCategory(
    const mozilla::MarkerCategory& aMarkerCategory,
    const mozilla::baseprofiler::ProfilingCategoryPair& aProfilingCategoryPair,
    const mozilla::baseprofiler::ProfilingCategory& aProfilingCategory) {
  MOZ_RELEASE_ASSERT(aMarkerCategory.CategoryPair() == aProfilingCategoryPair,
                     "Unexpected MarkerCategory::CategoryPair()");

  MOZ_RELEASE_ASSERT(
      mozilla::MarkerCategory(aProfilingCategoryPair).CategoryPair() ==
          aProfilingCategoryPair,
      "MarkerCategory(<name>).CategoryPair() should return <name>");

  MOZ_RELEASE_ASSERT(aMarkerCategory.GetCategory() == aProfilingCategory,
                     "Unexpected MarkerCategory::GetCategory()");

  mozilla::ProfileBufferChunkManagerSingle chunkManager(512);
  mozilla::ProfileChunkedBuffer buffer(
      mozilla::ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);
  mozilla::ProfileBufferBlockIndex i = buffer.PutObject(aMarkerCategory);
  MOZ_RELEASE_ASSERT(i != mozilla::ProfileBufferBlockIndex{},
                     "Failed serialization");
  buffer.ReadEach([&](mozilla::ProfileBufferEntryReader& aER,
                      mozilla::ProfileBufferBlockIndex aIndex) {
    MOZ_RELEASE_ASSERT(aIndex == i, "Unexpected deserialization index");
    const auto readCategory = aER.ReadObject<mozilla::MarkerCategory>();
    MOZ_RELEASE_ASSERT(aER.RemainingBytes() == 0,
                       "Unexpected extra serialized bytes");
    MOZ_RELEASE_ASSERT(readCategory.CategoryPair() == aProfilingCategoryPair,
                       "Incorrect deserialization value");
  });
}

void TestMarkerCategory() {
  printf("TestMarkerCategory...\n");

  mozilla::ProfileBufferChunkManagerSingle chunkManager(512);
  mozilla::ProfileChunkedBuffer buffer(
      mozilla::ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);

#  define CATEGORY_ENUM_BEGIN_CATEGORY(name, labelAsString, color)
#  define CATEGORY_ENUM_SUBCATEGORY(supercategory, name, labelAsString)     \
    static_assert(                                                          \
        std::is_same_v<decltype(mozilla::baseprofiler::category::name),     \
                       const mozilla::MarkerCategory>,                      \
        "baseprofiler::category::<name> should be a const MarkerCategory"); \
                                                                            \
    SubTestMarkerCategory(                                                  \
        mozilla::baseprofiler::category::name,                              \
        mozilla::baseprofiler::ProfilingCategoryPair::name,                 \
        mozilla::baseprofiler::ProfilingCategory::supercategory);
#  define CATEGORY_ENUM_END_CATEGORY
  MOZ_PROFILING_CATEGORY_LIST(CATEGORY_ENUM_BEGIN_CATEGORY,
                              CATEGORY_ENUM_SUBCATEGORY,
                              CATEGORY_ENUM_END_CATEGORY)
#  undef CATEGORY_ENUM_BEGIN_CATEGORY
#  undef CATEGORY_ENUM_SUBCATEGORY
#  undef CATEGORY_ENUM_END_CATEGORY

  printf("TestMarkerCategory done\n");
}

void TestMarkerThreadId() {
  printf("TestMarkerThreadId...\n");

  MOZ_RELEASE_ASSERT(MarkerThreadId{}.IsUnspecified());
  MOZ_RELEASE_ASSERT(!MarkerThreadId::MainThread().IsUnspecified());
  MOZ_RELEASE_ASSERT(!MarkerThreadId::CurrentThread().IsUnspecified());

  MOZ_RELEASE_ASSERT(!MarkerThreadId{
      mozilla::baseprofiler::BaseProfilerThreadId::FromNumber(42)}
                          .IsUnspecified());
  MOZ_RELEASE_ASSERT(
      MarkerThreadId{
          mozilla::baseprofiler::BaseProfilerThreadId::FromNumber(42)}
          .ThreadId()
          .ToNumber() == 42);

  // We'll assume that this test runs in the main thread (which should be true
  // when called from the `main` function).
  MOZ_RELEASE_ASSERT(MarkerThreadId::MainThread().ThreadId() ==
                     mozilla::baseprofiler::profiler_main_thread_id());

  MOZ_RELEASE_ASSERT(MarkerThreadId::CurrentThread().ThreadId() ==
                     mozilla::baseprofiler::profiler_current_thread_id());

  MOZ_RELEASE_ASSERT(MarkerThreadId::CurrentThread().ThreadId() ==
                     mozilla::baseprofiler::profiler_main_thread_id());

  std::thread testThread([]() {
    MOZ_RELEASE_ASSERT(!MarkerThreadId::MainThread().IsUnspecified());
    MOZ_RELEASE_ASSERT(!MarkerThreadId::CurrentThread().IsUnspecified());

    MOZ_RELEASE_ASSERT(MarkerThreadId::MainThread().ThreadId() ==
                       mozilla::baseprofiler::profiler_main_thread_id());

    MOZ_RELEASE_ASSERT(MarkerThreadId::CurrentThread().ThreadId() ==
                       mozilla::baseprofiler::profiler_current_thread_id());

    MOZ_RELEASE_ASSERT(MarkerThreadId::CurrentThread().ThreadId() !=
                       mozilla::baseprofiler::profiler_main_thread_id());
  });
  testThread.join();

  printf("TestMarkerThreadId done\n");
}

void TestMarkerNoPayload() {
  printf("TestMarkerNoPayload...\n");

  mozilla::ProfileBufferChunkManagerSingle chunkManager(512);
  mozilla::ProfileChunkedBuffer buffer(
      mozilla::ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);

  mozilla::ProfileBufferBlockIndex i0 =
      mozilla::baseprofiler::AddMarkerToBuffer(
          buffer, "literal", mozilla::baseprofiler::category::OTHER_Profiling);
  MOZ_RELEASE_ASSERT(i0);

  const std::string dynamic = "dynamic";
  mozilla::ProfileBufferBlockIndex i1 =
      mozilla::baseprofiler::AddMarkerToBuffer(
          buffer, dynamic,
          mozilla::baseprofiler::category::GRAPHICS_FlushingAsyncPaints, {});
  MOZ_RELEASE_ASSERT(i1);
  MOZ_RELEASE_ASSERT(i1 > i0);

  mozilla::ProfileBufferBlockIndex i2 =
      mozilla::baseprofiler::AddMarkerToBuffer(
          buffer, std::string_view("string_view"),
          mozilla::baseprofiler::category::GRAPHICS_FlushingAsyncPaints, {});
  MOZ_RELEASE_ASSERT(i2);
  MOZ_RELEASE_ASSERT(i2 > i1);

#  ifdef DEBUG
  buffer.Dump();
#  endif

  PrintMarkers(buffer);

  printf("TestMarkerNoPayload done\n");
}

void TestUserMarker() {
  printf("TestUserMarker...\n");

  // User-defined marker type with text.
  // It's fine to define it right in the function where it's used.
  struct MarkerTypeTestMinimal {
    static constexpr Span<const char> MarkerTypeName() {
      return MakeStringSpan("test-minimal");
    }
    static void StreamJSONMarkerData(
        mozilla::baseprofiler::SpliceableJSONWriter& aWriter,
        const std::string& aText) {
      aWriter.StringProperty("text", aText);
    }
    static mozilla::MarkerSchema MarkerTypeDisplay() {
      using MS = mozilla::MarkerSchema;
      MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
      schema.SetTooltipLabel("tooltip for test-minimal");
      schema.AddKeyLabelFormatSearchable("text", "Text", MS::Format::String,
                                         MS::Searchable::Searchable);
      return schema;
    }
  };

  mozilla::ProfileBufferChunkManagerSingle chunkManager(1024);
  mozilla::ProfileChunkedBuffer buffer(
      mozilla::ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling, {},
      MarkerTypeTestMinimal{}, std::string("payload text")));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerThreadId(
          mozilla::baseprofiler::BaseProfilerThreadId::FromNumber(123)),
      MarkerTypeTestMinimal{}, std::string("ThreadId(123)")));

  auto start = mozilla::TimeStamp::Now();

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerTiming::InstantAt(start), MarkerTypeTestMinimal{},
      std::string("InstantAt(start)")));

  auto then = mozilla::TimeStamp::Now();

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerTiming::IntervalStart(start), MarkerTypeTestMinimal{},
      std::string("IntervalStart(start)")));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerTiming::IntervalEnd(then), MarkerTypeTestMinimal{},
      std::string("IntervalEnd(then)")));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerTiming::Interval(start, then), MarkerTypeTestMinimal{},
      std::string("Interval(start, then)")));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerTiming::IntervalUntilNowFrom(start),
      MarkerTypeTestMinimal{}, std::string("IntervalUntilNowFrom(start)")));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerStack::NoStack(), MarkerTypeTestMinimal{},
      std::string("NoStack")));
  // Note: We cannot test stack-capture here, because the profiler is not
  // initialized.

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, "test2", mozilla::baseprofiler::category::OTHER_Profiling,
      mozilla::MarkerInnerWindowId(123), MarkerTypeTestMinimal{},
      std::string("InnerWindowId(123)")));

#  ifdef DEBUG
  buffer.Dump();
#  endif

  PrintMarkers(buffer);

  printf("TestUserMarker done\n");
}

void TestPredefinedMarkers() {
  printf("TestPredefinedMarkers...\n");

  mozilla::ProfileBufferChunkManagerSingle chunkManager(1024);
  mozilla::ProfileChunkedBuffer buffer(
      mozilla::ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, std::string_view("tracing"),
      mozilla::baseprofiler::category::OTHER, {},
      mozilla::baseprofiler::markers::Tracing{}, "category"));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, std::string_view("text"), mozilla::baseprofiler::category::OTHER,
      {}, mozilla::baseprofiler::markers::TextMarker{}, "text text"));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, std::string_view("media"), mozilla::baseprofiler::category::OTHER,
      {}, mozilla::baseprofiler::markers::MediaSampleMarker{}, 123, 456, 789));

  MOZ_RELEASE_ASSERT(mozilla::baseprofiler::AddMarkerToBuffer(
      buffer, std::string_view("media"), mozilla::baseprofiler::category::OTHER,
      {}, mozilla::baseprofiler::markers::VideoFallingBehindMarker{}, 123,
      456));

#  ifdef DEBUG
  buffer.Dump();
#  endif

  PrintMarkers(buffer);

  printf("TestPredefinedMarkers done\n");
}

void TestProfilerMarkers() {
  printf(
      "TestProfilerMarkers -- pid: %" PRIu64 ", tid: %" PRIu64 "\n",
      uint64_t(mozilla::baseprofiler::profiler_current_process_id().ToNumber()),
      uint64_t(mozilla::baseprofiler::profiler_current_thread_id().ToNumber()));
  // ::SleepMilli(10000);

  TestUniqueJSONStrings();
  TestMarkerCategory();
  TestMarkerThreadId();
  TestMarkerNoPayload();
  TestUserMarker();
  TestPredefinedMarkers();

  printf("TestProfilerMarkers done\n");
}

#else  // MOZ_GECKO_PROFILER

// Testing that macros are still #defined (but do nothing) when
// MOZ_GECKO_PROFILER is disabled.
void TestProfiler() {
  // These don't need to make sense, we just want to know that they're defined
  // and don't do anything.

#  ifndef AUTO_BASE_PROFILER_INIT
#    error AUTO_BASE_PROFILER_INIT not #defined
#  endif  // AUTO_BASE_PROFILER_INIT
  AUTO_BASE_PROFILER_INIT;

#  ifndef AUTO_BASE_PROFILER_MARKER_TEXT
#    error AUTO_BASE_PROFILER_MARKER_TEXT not #defined
#  endif  // AUTO_BASE_PROFILER_MARKER_TEXT

#  ifndef AUTO_BASE_PROFILER_LABEL
#    error AUTO_BASE_PROFILER_LABEL not #defined
#  endif  // AUTO_BASE_PROFILER_LABEL

#  ifndef AUTO_BASE_PROFILER_THREAD_SLEEP
#    error AUTO_BASE_PROFILER_THREAD_SLEEP not #defined
#  endif  // AUTO_BASE_PROFILER_THREAD_SLEEP
  AUTO_BASE_PROFILER_THREAD_SLEEP;

#  ifndef BASE_PROFILER_MARKER_UNTYPED
#    error BASE_PROFILER_MARKER_UNTYPED not #defined
#  endif  // BASE_PROFILER_MARKER_UNTYPED

#  ifndef BASE_PROFILER_MARKER
#    error BASE_PROFILER_MARKER not #defined
#  endif  // BASE_PROFILER_MARKER

#  ifndef BASE_PROFILER_MARKER_TEXT
#    error BASE_PROFILER_MARKER_TEXT not #defined
#  endif  // BASE_PROFILER_MARKER_TEXT

  MOZ_RELEASE_ASSERT(!mozilla::baseprofiler::profiler_get_backtrace(),
                     "profiler_get_backtrace should return nullptr");
  mozilla::ProfileChunkedBuffer buffer(
      mozilla::ProfileChunkedBuffer::ThreadSafety::WithoutMutex);
  MOZ_RELEASE_ASSERT(!mozilla::baseprofiler::profiler_capture_backtrace_into(
                         buffer, mozilla::StackCaptureOptions::Full),
                     "profiler_capture_backtrace_into should return false");
  MOZ_RELEASE_ASSERT(!mozilla::baseprofiler::profiler_capture_backtrace(),
                     "profiler_capture_backtrace should return nullptr");
}

// Testing that macros are still #defined (but do nothing) when
// MOZ_GECKO_PROFILER is disabled.
void TestProfilerMarkers() {
  // These don't need to make sense, we just want to know that they're defined
  // and don't do anything.
}

#endif  // MOZ_GECKO_PROFILER else

#if defined(XP_WIN)
int wmain()
#else
int main()
#endif  // defined(XP_WIN)
{
#ifdef MOZ_GECKO_PROFILER
  printf("BaseTestProfiler -- pid: %" PRIu64 ", tid: %" PRIu64 "\n",
         uint64_t(baseprofiler::profiler_current_process_id().ToNumber()),
         uint64_t(baseprofiler::profiler_current_thread_id().ToNumber()));
  // ::SleepMilli(10000);
#endif  // MOZ_GECKO_PROFILER

  TestFailureLatch();
  TestProfilerUtils();
  TestBaseAndProfilerDetail();
  TestSharedMutex();
  TestProportionValue();
  TestProgressLogger();
  // Note that there are two `TestProfiler{,Markers}` functions above, depending
  // on whether MOZ_GECKO_PROFILER is #defined.
  {
    printf("profiler_init()...\n");
    AUTO_BASE_PROFILER_INIT;

    TestProfiler();
    TestProfilerMarkers();
  }

  return 0;
}
