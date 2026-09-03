// Copyright (c) 2026 Maged Michael
// See LICENSE for licensing terms.

#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

// For asymmetric fence only
#include <cstdlib>
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>

/// Asymmetric Memory Fences

namespace p1202 {

inline void asymmetric_thread_fence_light() noexcept {
  std::atomic_signal_fence(std::memory_order::seq_cst);
}

inline void asymmetric_thread_fence_heavy() noexcept {
  static const bool supported = ::syscall(SYS_membarrier,
      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
  // This implementation assumes membarrier support.
  // For portable implementations, see folly AsymmetricThreadFence.
  if (!supported) std::abort();
  ::syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
}

} // namespace p1202

/// Forward Declarations

namespace std {
template <class T, class D = default_delete<T>> class hazard_pointer_obj_base;
class hazard_pointer;
}

namespace mm_hp_detail {
class hp_obj;
struct hp_rec;
struct hp_tc;
struct hp_tc_flush_guard;
struct hp_domain;
struct hp_domain_exit_guard;

hp_rec* hp_domain_acquire_rec();
void hp_domain_release_rec_list(hp_rec* head, hp_rec* tail) noexcept;
void hp_domain_retire_obj(hp_obj* obj) noexcept;
void hp_tc_flush() noexcept;
void hp_tc_register_flush();
hp_rec* hp_acquire_rec();
}

/// Details Required by std

namespace mm_hp_detail {

/// Concepts

template <class T, class D>
auto hazard_protectable_test(std::hazard_pointer_obj_base<T, D>* base)
    -> decltype(static_cast<T*>(base));

template <class T>
concept hazard_protectable = requires(T* ptr) { hazard_protectable_test<T>(ptr); };

template <class T>
void assert_hazard_protectable() noexcept {
  static_assert(hazard_protectable<T>, "T is not a hazard-protectable type");
}

/// Classes

class hp_obj {
  hp_obj* next_;
  void (*reclaim_)(hp_obj*);
  void* reserved_;  // Future ABI (pointer to P3427 cohort)

  template <class T, class D> friend class std::hazard_pointer_obj_base;
  friend struct hp_domain;
}; // class hp_obj

struct alignas(128) hp_rec {
  std::atomic<const hp_obj*> hp_{nullptr};
  hp_rec* next_{nullptr};
  hp_rec* avail_next_{nullptr};
}; // struct hp_rec

static_assert(alignof(hp_rec) > 1);  // hp_domain::avail_ packs a lock in bit 0

/// Thread-Local Cache

struct hp_tc {
  static constexpr int k_max_capacity = 126;  // ABI max capacity
  static_assert(k_max_capacity < 256);        // capacity_ and count_ are uint8_t
  hp_rec* fast_{nullptr};
  std::uint8_t capacity_{8};
  std::uint8_t count_{0};
  bool closed_{false};
  std::array<hp_rec*, k_max_capacity> hp_recs_;

  hp_rec* pop() noexcept {
    hp_rec* rec = fast_;
    if (rec) {
      fast_ = nullptr;
      return rec;
    }
    if (count_ == 0) return nullptr;
    rec = hp_recs_[--count_];
    assert(rec);
    return rec;
  }

  bool push(hp_rec* rec) noexcept {
    assert(rec);
    if (closed_) return false;
    if (!fast_) {
      fast_ = rec;
      return true;
    }
    if (count_ == capacity_) return false;
    hp_recs_[count_++] = rec;
    return true;
  }
}; // struct hp_tc

struct hp_tc_flush_guard {
  ~hp_tc_flush_guard() noexcept { hp_tc_flush(); }
}; // struct hp_tc_flush_guard

inline constinit thread_local hp_tc t_tc{};

inline void hp_tc_flush() noexcept {
  assert(!t_tc.closed_);
  assert(t_tc.count_ <= t_tc.capacity_);
  hp_rec* head = t_tc.fast_;
  hp_rec* tail = head;
  for (std::uint8_t i = 0; i < t_tc.count_; ++i) {
    hp_rec* rec = t_tc.hp_recs_[i];
    assert(rec);
    if (!head) tail = rec;
    rec->avail_next_ = head;
    head = rec;
  }
  if (head) {
    hp_domain_release_rec_list(head, tail);
  }
  t_tc.fast_ = nullptr;
  t_tc.count_ = 0;
  t_tc.closed_ = true;
}

inline void hp_tc_register_flush() {
  static thread_local hp_tc_flush_guard t_flush_guard;
}

[[gnu::noinline]]
inline hp_rec* hp_acquire_rec() {
  if (!t_tc.closed_) hp_tc_register_flush();
  return hp_domain_acquire_rec();
}

} // namespace mm_hp_detail

/// Standard Classes and Functions

namespace std {

template <class T, class D>
class hazard_pointer_obj_base : public mm_hp_detail::hp_obj {
  [[no_unique_address]] D deleter_;

 public:
  void retire(D d = D()) noexcept {
    mm_hp_detail::assert_hazard_protectable<T>();
    deleter_ = std::move(d);
    reclaim_ = [](mm_hp_detail::hp_obj* p) {
      auto* self = static_cast<hazard_pointer_obj_base*>(p);
      self->deleter_(static_cast<T*>(self));
    };
    mm_hp_detail::hp_domain_retire_obj(this);
  }

 protected:
  hazard_pointer_obj_base() = default;
  hazard_pointer_obj_base(const hazard_pointer_obj_base&) = default;
  hazard_pointer_obj_base(hazard_pointer_obj_base&&) = default;
  hazard_pointer_obj_base& operator=(const hazard_pointer_obj_base&) = default;
  hazard_pointer_obj_base& operator=(hazard_pointer_obj_base&&) = default;
  ~hazard_pointer_obj_base() = default;
}; // class hazard_pointer_obj_base

class hazard_pointer {
  mm_hp_detail::hp_rec* hp_rec_{nullptr};

  friend hazard_pointer make_hazard_pointer();

  explicit hazard_pointer(mm_hp_detail::hp_rec* rec) noexcept : hp_rec_(rec) {}

 public:
  hazard_pointer() noexcept = default;  // Compatible with future constexpr ABI

  hazard_pointer(hazard_pointer&& hp) noexcept
      : hp_rec_(std::exchange(hp.hp_rec_, nullptr)) {}

  hazard_pointer& operator=(hazard_pointer&& hp) noexcept {
    hazard_pointer(std::move(hp)).swap(*this);
    return *this;
  }

  ~hazard_pointer() {
    if (hp_rec_) {
      reset_protection();
      if (!mm_hp_detail::t_tc.push(hp_rec_)) {
        mm_hp_detail::hp_domain_release_rec_list(hp_rec_, hp_rec_);
      }
    }
  }

  bool empty() const noexcept { return hp_rec_ == nullptr; }

  template <class T>
  T* protect(const atomic<T*>& src) noexcept {
    assert(!empty());
    T* ptr = src.load(memory_order::relaxed);
    while (!try_protect(ptr, src)) {}
    return ptr;
  }

  template <class T>
  bool try_protect(T*& ptr, const atomic<T*>& src) noexcept {
    assert(!empty());
    T* expected = ptr;
    reset_protection(expected);
    p1202::asymmetric_thread_fence_light();
    ptr = src.load(memory_order::acquire);
    if (std::memcmp(&ptr, &expected, sizeof(ptr)) != 0) {
      reset_protection();
      return false;
    }
    return true;
  }

  template <class T>
  void reset_protection(const T* ptr) noexcept {
    assert(!empty());
    mm_hp_detail::assert_hazard_protectable<T>();
    const auto* hp_base = static_cast<const mm_hp_detail::hp_obj*>(ptr);
    hp_rec_->hp_.store(hp_base, memory_order::release);
  }

  void reset_protection(nullptr_t = nullptr) noexcept {
    assert(!empty());
    hp_rec_->hp_.store(nullptr, memory_order::release);
  }

  void swap(hazard_pointer& hp) noexcept { std::swap(hp_rec_, hp.hp_rec_); }
}; // class hazard_pointer

/// Free Functions

inline void swap(hazard_pointer& a, hazard_pointer& b) noexcept { a.swap(b); }

inline hazard_pointer make_hazard_pointer() {
  mm_hp_detail::hp_rec* rec = mm_hp_detail::t_tc.pop();
  if (!rec) rec = mm_hp_detail::hp_acquire_rec();
  return hazard_pointer(rec);
}

} // namespace std

/// Domain Details

namespace mm_hp_detail {

using mo = std::memory_order;

inline constinit thread_local bool t_reclaiming{false};

struct hp_domain {
  static constexpr std::size_t k_reclaim_floor = 1000;

  alignas(128) std::atomic<std::uintptr_t> avail_{0};
  alignas(128) std::atomic<std::size_t> rcount_{0};
               std::atomic<hp_obj*> retired_{nullptr};
  alignas(128) std::atomic<hp_rec*> hp_recs_{nullptr};
               std::atomic<std::size_t> hcount_{0};
  alignas(128) std::array<std::byte, 5 * 128> reserved_;  // future ABI

  struct hp_obj_list {
    hp_obj* head{nullptr};
    hp_obj* tail{nullptr};
    std::size_t count{0};

    void add(hp_obj* obj) noexcept {
      if (!head) tail = obj;
      obj->next_ = head;
      head = obj;
      ++count;
    }
  }; // struct hp_obj_list

  /// HP Record Operations

  hp_rec* acquire_hp_rec() {
    while (true) {
      std::uintptr_t avail = avail_.load(mo::relaxed);
      if (avail & 1) {  // locked
        std::this_thread::yield();
        continue;
      }
      if (!avail) return new_hp_rec();
      if (avail_.compare_exchange_weak(
          avail, avail | 1, mo::acquire, mo::relaxed)) {
        hp_rec* rec = reinterpret_cast<hp_rec*>(avail);
        avail_.store(reinterpret_cast<std::uintptr_t>(rec->avail_next_), mo::release);
        rec->avail_next_ = nullptr;
        return rec;
      }
    }
  }

  void release_hp_rec_list(hp_rec* head, hp_rec* tail) noexcept {
    assert(head);
    assert(tail);
    std::uintptr_t newhead = reinterpret_cast<std::uintptr_t>(head);
    while (true) {
      std::uintptr_t avail = avail_.load(mo::relaxed);
      if (avail & 1) {  // locked
        std::this_thread::yield();
        continue;
      }
      tail->avail_next_ = reinterpret_cast<hp_rec*>(avail);
      if (avail_.compare_exchange_weak(avail, newhead, mo::release, mo::relaxed)) return;
    }
  }

  hp_rec* new_hp_rec() {
    hp_rec* rec = new hp_rec();
    hp_rec* head = hp_recs_.load(mo::relaxed);
    do {
      rec->next_ = head;
    } while (!hp_recs_.compare_exchange_weak(head, rec, mo::release, mo::relaxed));
    hcount_.fetch_add(1, mo::relaxed);
    return rec;
  }

  /// Retired Object Operations

  void push_retired_obj(hp_obj* obj) noexcept {
    assert(obj);
    assert(obj->reclaim_);
    p1202::asymmetric_thread_fence_light();
    if (push_retired_list({obj, obj, 1}) >= reclaim_threshold() && !t_reclaiming) {
      do_reclamation();
    }
  }

  std::size_t push_retired_list(const hp_obj_list& list) noexcept {
    assert(list.head);
    assert(list.tail);
    assert(list.count);
    hp_obj* cur = retired_.load(mo::relaxed);
    do {
      list.tail->next_ = cur;
    } while (!retired_.compare_exchange_weak(cur, list.head, mo::release, mo::relaxed));
    return rcount_.fetch_add(list.count, mo::relaxed) + list.count;
  }

  /// Reclamation Operations

  std::size_t reclaim_threshold() const noexcept {
    std::size_t hcount = hcount_.load(mo::relaxed);
    std::size_t scaled = 2 * hcount;
    return scaled < k_reclaim_floor ? k_reclaim_floor : scaled;
  }

  [[gnu::noinline]]
  void do_reclamation() noexcept {
    hp_obj* retired = retired_.exchange(nullptr, mo::acquire);
    if (!retired) return;
    t_reclaiming = true;
    rcount_.store(0, mo::relaxed);
    p1202::asymmetric_thread_fence_heavy();
    // Must load hp_recs_ after the fence. Earlier load could miss a protecting hp_rec.
    reclaim_unprotected(retired, hp_recs_.load(mo::acquire));
    t_reclaiming = false;
  }

  void reclaim_unprotected(hp_obj* retired, hp_rec* hp_recs) noexcept {
    hp_obj_list keep;
    while (retired) {
      try {
        reclaim_with_set(retired, collect_protected(hp_recs), keep);
        break;
      } catch (const std::bad_alloc&) {
        retired = reclaim_one_in_place(retired, hp_recs, keep);
      }
    }
    if (keep.head) push_retired_list(keep);
  }

  void reclaim_with_set(hp_obj* retired,
                        const std::unordered_set<const hp_obj*>& protected_set,
                        hp_obj_list& keep) noexcept {
    while (retired) {
      hp_obj* next = retired->next_;
      if (protected_set.contains(retired)) keep.add(retired);
      else retired->reclaim_(retired);
      retired = next;
    }
  }

  hp_obj* reclaim_one_in_place(hp_obj* retired, hp_rec* hp_recs,
                               hp_obj_list& keep) noexcept {
    while (retired) {
      hp_obj* next = retired->next_;
      if (!is_protected(retired, hp_recs)) {
        retired->reclaim_(retired);
        return next;
      }
      keep.add(retired);
      retired = next;
    }
    return nullptr;
  }

  std::unordered_set<const hp_obj*> collect_protected(hp_rec* hp_recs) {
    std::unordered_set<const hp_obj*> protected_set;
    protected_set.reserve(hcount_.load(mo::relaxed));
    for (hp_rec* rec = hp_recs; rec; rec = rec->next_) {
      const hp_obj* ptr = rec->hp_.load(mo::relaxed);
      if (ptr) protected_set.insert(ptr);
    }
    return protected_set;
  }

  bool is_protected(hp_obj* obj, hp_rec* hp_recs) noexcept {
    for (hp_rec* rec = hp_recs; rec; rec = rec->next_) {
      if (rec->hp_.load(mo::relaxed) == obj) return true;
    }
    return false;
  }

}; // struct hp_domain

/// Domain Objects and Free Functions

static_assert(std::is_trivially_destructible_v<hp_domain>);

inline constinit hp_domain g_domain{};

inline hp_rec* hp_domain_acquire_rec() {
  return g_domain.acquire_hp_rec();
}

[[gnu::noinline]]
inline void hp_domain_release_rec_list(hp_rec* head, hp_rec* tail) noexcept {
  g_domain.release_hp_rec_list(head, tail);
}

inline void hp_domain_retire_obj(hp_obj* obj) noexcept {
  g_domain.push_retired_obj(obj);
}

/// Domain Exit Guard

struct hp_domain_exit_guard {
  ~hp_domain_exit_guard() noexcept { g_domain.do_reclamation(); }
}; // struct hp_domain_exit_guard

inline hp_domain_exit_guard g_domain_exit_guard;

} // namespace mm_hp_detail
