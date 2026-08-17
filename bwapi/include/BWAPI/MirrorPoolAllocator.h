#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>

// MirrorPoolAllocator — a thread-local, size-classed free-list allocator for BWAPI's SetContainers
// (Unitset query results and the mirror's per-frame unit sets). It RECYCLES freed blocks into
// per-size-class free-lists instead of returning them to malloc, killing the per-frame malloc/free
// churn that dominates this memory-latency-bound engine (getUnitsInRectangle alone allocated a fresh
// std::unordered_set on every call — thousands of malloc/free pairs per frame).
//
// SAFETY / CORRECTNESS:
//   * Lifetime-safe. This is NOT a frame-reset arena: memory returns to the pool on deallocate(), so
//     return-by-value (getUnitsInRectangle) and any cross-frame hold of a Unitset are correct.
//   * Byte-exact. Only memory PROVENANCE changes. A std::unordered_set's iteration order is a function
//     of the hash (StableUnitHash), the bucket count (rehash policy) and per-bucket insertion order —
//     none depend on the address the allocator returns — so the game/command stream is identical.
//     The kill-switch below is the differential-test knob (rule 14): BOT_MIRROR_POOL=0 delegates
//     every allocate/deallocate to ::operator new/delete (exactly std::allocator), and pool-on must
//     produce byte-identical digests to pool-off.
//   * Thread-local. The two dual-host lanes each get their own pool; they never share pool state, so
//     concurrent dispatch (if ever re-enabled) is race-free. A block allocated on one thread and
//     freed on another is still memory-safe (it simply joins the freeing thread's free-list).
//   * Stateless from the container's view (the pool is a thread_local singleton; the allocator is an
//     empty handle, all instances compare equal) so moves/swaps never reallocate.
//
// Blocks larger than kMaxPooled bypass the pool (straight ::operator new/delete), bounding pool
// growth to the working set of small allocations (the mirror is bounded to ~1700 units). Pooled
// blocks are intentionally not returned to the OS at thread exit — the process is exiting; the OS
// reclaims them (standard pool behaviour).

namespace BWAPI {
namespace detail {

class MirrorPool {
  static constexpr std::size_t kAlign     = 16;              // block granularity / min size
  static constexpr std::size_t kMaxPooled = 512;             // bytes; larger requests bypass the pool
  static constexpr std::size_t kClasses   = kMaxPooled / kAlign;
  void* freelist_[kClasses] = {};
  const bool enabled_;

  static bool read_enabled() {
    const char* v = std::getenv("BOT_MIRROR_POOL");
    return !(v && v[0] == '0');
  }
  static std::size_t round_up(std::size_t n) { return ((n + kAlign - 1) / kAlign) * kAlign; }

public:
  MirrorPool() : enabled_(read_enabled()) {}

  void* allocate(std::size_t n) {
    if (n == 0) n = 1;
    if (!enabled_ || n > kMaxPooled) return ::operator new(n);
    const std::size_t rounded = round_up(n);
    void*& head = freelist_[(rounded / kAlign) - 1];
    if (head) { void* p = head; head = *static_cast<void**>(p); return p; }
    return ::operator new(rounded);   // fresh block sized to the class → free-list stays homogeneous
  }

  void deallocate(void* p, std::size_t n) noexcept {
    if (!p) return;
    if (n == 0) n = 1;
    if (!enabled_ || n > kMaxPooled) { ::operator delete(p); return; }
    const std::size_t rounded = round_up(n);
    void*& head = freelist_[(rounded / kAlign) - 1];
    *static_cast<void**>(p) = head;   // intrusive next-pointer (blocks are >= kAlign >= sizeof(void*))
    head = p;
  }
};

inline MirrorPool& mirror_pool() {
  // initial-exec TLS model: the general-dynamic default costs a __tls_get_addr call per pool
  // access (callgrind 2026-08-13: 83K Ir/frame on the ladder ISA — the single largest linkage
  // cost). libBWAPI/libBWAPILIB are load-time dependencies of the launcher (never dlopen'd), so
  // initial-exec is legal and binds the slot at load. No-op on toolchains without the attribute.
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((tls_model("initial-exec"))) static thread_local MirrorPool pool;
#else
  static thread_local MirrorPool pool;
#endif
  return pool;
}

}  // namespace detail

template <class T>
struct MirrorPoolAllocator {
  using value_type = T;
  MirrorPoolAllocator() noexcept = default;
  template <class U> MirrorPoolAllocator(const MirrorPoolAllocator<U>&) noexcept {}

  T* allocate(std::size_t nT) {
    return static_cast<T*>(detail::mirror_pool().allocate(nT * sizeof(T)));
  }
  void deallocate(T* p, std::size_t nT) noexcept {
    detail::mirror_pool().deallocate(p, nT * sizeof(T));
  }

  template <class U> bool operator==(const MirrorPoolAllocator<U>&) const noexcept { return true; }
  template <class U> bool operator!=(const MirrorPoolAllocator<U>&) const noexcept { return false; }
};

}  // namespace BWAPI
