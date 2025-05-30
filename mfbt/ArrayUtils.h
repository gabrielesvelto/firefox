/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implements various helper functions related to arrays.
 */

#ifndef mozilla_ArrayUtils_h
#define mozilla_ArrayUtils_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#  include <algorithm>
#  include <array>
#  include <type_traits>

#  include "mozilla/Alignment.h"

namespace mozilla {

/*
 * Safely subtract two pointers when it is known that aEnd >= aBegin, yielding a
 * size_t result.
 *
 * Ordinary pointer subtraction yields a ptrdiff_t result, which, being signed,
 * has insufficient range to express the distance between pointers at opposite
 * ends of the address space. Furthermore, most compilers use ptrdiff_t to
 * represent the intermediate byte address distance, before dividing by
 * sizeof(T); if that intermediate result overflows, they'll produce results
 * with the wrong sign even when the correct scaled distance would fit in a
 * ptrdiff_t.
 */
template <class T>
MOZ_ALWAYS_INLINE size_t PointerRangeSize(T* aBegin, T* aEnd) {
  MOZ_ASSERT(aEnd >= aBegin);
  return (size_t(aEnd) - size_t(aBegin)) / sizeof(T);
}

/**
 * std::equal has subpar ergonomics.
 */

template <typename T, typename U, size_t N>
bool ArrayEqual(const T (&a)[N], const U (&b)[N]) {
  return std::equal(a, a + N, b);
}

template <typename T, typename U>
bool ArrayEqual(const T* const a, const U* const b, const size_t n) {
  return std::equal(a, a + n, b);
}

namespace detail {

template <typename AlignType, typename Pointee, typename = void>
struct AlignedChecker {
  static void test(const Pointee* aPtr) {
    MOZ_ASSERT((uintptr_t(aPtr) % alignof(AlignType)) == 0,
               "performing a range-check with a misaligned pointer");
  }
};

template <typename AlignType, typename Pointee>
struct AlignedChecker<AlignType, Pointee,
                      std::enable_if_t<std::is_void_v<AlignType>>> {
  static void test(const Pointee* aPtr) {}
};

}  // namespace detail

/**
 * Determines whether |aPtr| points at an object in the range [aBegin, aEnd).
 *
 * |aPtr| must have the same alignment as |aBegin| and |aEnd|.  This usually
 * should be achieved by ensuring |aPtr| points at a |U|, not just that it
 * points at a |T|.
 *
 * It is a usage error for any argument to be misaligned.
 *
 * It's okay for T* to be void*, and if so U* may also be void*.  In the latter
 * case no argument is required to be aligned (obviously, as void* implies no
 * particular alignment).
 */
template <typename T, typename U>
inline std::enable_if_t<std::is_same_v<T, U> || std::is_base_of<T, U>::value ||
                            std::is_void_v<T>,
                        bool>
IsInRange(const T* aPtr, const U* aBegin, const U* aEnd) {
  MOZ_ASSERT(aBegin <= aEnd);
  detail::AlignedChecker<U, T>::test(aPtr);
  detail::AlignedChecker<U, U>::test(aBegin);
  detail::AlignedChecker<U, U>::test(aEnd);
  return aBegin <= reinterpret_cast<const U*>(aPtr) &&
         reinterpret_cast<const U*>(aPtr) < aEnd;
}

/**
 * Convenience version of the above method when the valid range is specified as
 * uintptr_t values.  As above, |aPtr| must be aligned, and |aBegin| and |aEnd|
 * must be aligned with respect to |T|.
 */
template <typename T>
inline bool IsInRange(const T* aPtr, uintptr_t aBegin, uintptr_t aEnd) {
  return IsInRange(aPtr, reinterpret_cast<const T*>(aBegin),
                   reinterpret_cast<const T*>(aEnd));
}

} /* namespace mozilla */

#endif /* __cplusplus */

#endif /* mozilla_ArrayUtils_h */
