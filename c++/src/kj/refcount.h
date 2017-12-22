// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef KJ_REFCOUNT_H_
#define KJ_REFCOUNT_H_

#include "memory.h"

#if defined(__GNUC__) && !KJ_HEADER_WARNINGS
#pragma GCC system_header
#endif

#if _MSC_VER
#if _MSC_VER < 1910
#include <intrin.h>
#else
#include <intrin0.h>
#endif
#endif

namespace kj {

// =======================================================================================
// Non-atomic (thread-unsafe) refcounting

class Refcounted: private Disposer {
  // Subclass this to create a class that contains a reference count. Then, use
  // `kj::refcounted<T>()` to allocate a new refcounted pointer.
  //
  // Do NOT use this lightly.  Refcounting is a crutch.  Good designs should strive to make object
  // ownership clear, so that refcounting is not necessary.  All that said, reference counting can
  // sometimes simplify code that would otherwise become convoluted with explicit ownership, even
  // when ownership relationships are clear at an abstract level.
  //
  // NOT THREADSAFE:  This refcounting implementation assumes that an object's references are
  // manipulated only in one thread, because atomic (thread-safe) refcounting is surprisingly slow.
  //
  // In general, abstract classes should _not_ subclass this.  The concrete class at the bottom
  // of the hierarchy should be the one to decide how it implements refcounting.  Interfaces should
  // expose only an `addRef()` method that returns `Own<InterfaceType>`.  There are two reasons for
  // this rule:
  // 1. Interfaces would need to virtually inherit Refcounted, otherwise two refcounted interfaces
  //    could not be inherited by the same subclass.  Virtual inheritance is awkward and
  //    inefficient.
  // 2. An implementation may decide that it would rather return a copy than a refcount, or use
  //    some other strategy.
  //
  // TODO(cleanup):  Rethink above.  Virtual inheritance is not necessarily that bad.  OTOH, a
  //   virtual function call for every refcount is sad in its own way.  A Ref<T> type to replace
  //   Own<T> could also be nice.

public:
  virtual ~Refcounted() noexcept(false);

  inline bool isShared() const { return refcount > 1; }
  // Check if there are multiple references to this object. This is sometimes useful for deciding
  // whether it's safe to modify the object vs. make a copy.

private:
  mutable uint refcount = 0;
  // "mutable" because disposeImpl() is const.  Bleh.

  void disposeImpl(void* pointer) const override;
  template <typename T>
  static Own<T> addRefInternal(T* object);

  template <typename T>
  friend Own<T> addRef(T& object);
  template <typename T, typename... Params>
  friend Own<T> refcounted(Params&&... params);
};

template <typename T, typename... Params>
inline Own<T> refcounted(Params&&... params) {
  // Allocate a new refcounted instance of T, passing `params` to its constructor.  Returns an
  // initial reference to the object.  More references can be created with `kj::addRef()`.

  return Refcounted::addRefInternal(new T(kj::fwd<Params>(params)...));
}

template <typename T>
Own<T> addRef(T& object) {
  // Return a new reference to `object`, which must subclass Refcounted and have been allocated
  // using `kj::refcounted<>()`.  It is suggested that subclasses implement a non-static addRef()
  // method which wraps this and returns the appropriate type.

  KJ_IREQUIRE(object.Refcounted::refcount > 0, "Object not allocated with kj::refcounted().");
  return Refcounted::addRefInternal(&object);
}

template <typename T>
Own<T> Refcounted::addRefInternal(T* object) {
  Refcounted* refcounted = object;
  ++refcounted->refcount;
  return Own<T>(object, *refcounted);
}

// =======================================================================================
// Atomic (thread-safe) refcounting
//
// Warning: Atomic ops are SLOW.

#if _MSC_VER
#if _M_ARM
#define KJ_MSVC_INTERLOCKED(OP, MEM) _Interlocked##OP##_##MEM
#else
#define KJ_MSVC_INTERLOCKED(OP, MEM) _Interlocked##OP
#endif
#endif

class AtomicRefcounted: private kj::Disposer {
public:
  virtual ~AtomicRefcounted() noexcept(false);

  inline bool isShared() const {
#if _MSC_VER
    KJ_MSVC_INTERLOCKED(Or, acq)(&refcount, 0) > 1;
#else
    return __atomic_load_n(&refcount, __ATOMIC_ACQUIRE) > 1;
#endif
  }

private:
#if _MSC_VER
  mutable volatile long refcount = 0;
#else
  mutable volatile uint refcount = 0;
#endif

  bool addRefWeakInternal() const;

  void disposeImpl(void* pointer) const override;
  template <typename T>
  static kj::Own<T> addRefInternal(T* object);
  template <typename T>
  static kj::Own<const T> addRefInternal(const T* object);

  template <typename T>
  friend kj::Own<T> atomicAddRef(T& object);
  template <typename T>
  friend kj::Own<const T> atomicAddRef(const T& object);
  template <typename T>
  friend kj::Maybe<kj::Own<const T>> atomicAddRefWeak(const T& object);
  template <typename T, typename... Params>
  friend kj::Own<T> atomicRefcounted(Params&&... params);
};

template <typename T, typename... Params>
inline kj::Own<T> atomicRefcounted(Params&&... params) {
  return AtomicRefcounted::addRefInternal(new T(kj::fwd<Params>(params)...));
}

template <typename T>
kj::Own<T> atomicAddRef(T& object) {
  KJ_IREQUIRE(object.AtomicRefcounted::refcount > 0, "Object not allocated with kj::refcounted().");
  return AtomicRefcounted::addRefInternal(&object);
}

template <typename T>
kj::Own<const T> atomicAddRef(const T& object) {
  KJ_IREQUIRE(object.AtomicRefcounted::refcount > 0, "Object not allocated with kj::refcounted().");
  return AtomicRefcounted::addRefInternal(&object);
}

template <typename T>
kj::Maybe<kj::Own<const T>> atomicAddRefWeak(const T& object) {
  // Try to addref an object whose refcount could have already reached zero in another thread, and
  // whose destructor could therefore already have started executing. The destructor must contain
  // some synchronization that guarantees that said destructor has not yet completed when
  // attomicAddRefWeak() is called (so that the object is still valid). Since the destructor cannot
  // be canceled once it has started, in the case that it has already started, this function
  // returns nullptr.

  const AtomicRefcounted* refcounted = &object;
  if (refcounted->addRefWeakInternal()) {
    return kj::Own<const T>(&object, *refcounted);
  } else {
    return nullptr;
  }
}

template <typename T>
kj::Own<T> AtomicRefcounted::addRefInternal(T* object) {
  AtomicRefcounted* refcounted = object;
#if _MSC_VER
  KJ_MSVC_INTERLOCKED(Increment, nf)(&refcounted->refcount);
#else
  __atomic_add_fetch(&refcounted->refcount, 1, __ATOMIC_RELAXED);
#endif
  return kj::Own<T>(object, *refcounted);
}

template <typename T>
kj::Own<const T> AtomicRefcounted::addRefInternal(const T* object) {
  const AtomicRefcounted* refcounted = object;
#if _MSC_VER
  KJ_MSVC_INTERLOCKED(Increment, nf)(&refcounted->refcount);
#else
  __atomic_add_fetch(&refcounted->refcount, 1, __ATOMIC_RELAXED);
#endif
  return kj::Own<const T>(object, *refcounted);
}

}  // namespace kj

#endif  // KJ_REFCOUNT_H_
