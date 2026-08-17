#pragma once
#include <unordered_set>
#include <functional>
#include "MirrorPoolAllocator.h"

namespace BWAPI
{
  // The set nodes + bucket arrays come from a thread-local recycling pool (MirrorPoolAllocator)
  // rather than malloc, which removes the per-frame allocator churn that dominates this
  // memory-latency-bound engine. Byte-exact (order depends on hash/buckets, not addresses);
  // kill-switch BOT_MIRROR_POOL=0 falls back to the system allocator. See MirrorPoolAllocator.h.
  template <class T, class HashT>
  using SetContainerUnderlyingT =
      std::unordered_set < T, HashT, std::equal_to<T>, MirrorPoolAllocator<T> >;

  /// <summary>This container is used to wrap convenience functions for BWAPI and be used as a
  /// bridge with a built-in set type.</summary>
  ///
  /// @tparam T
  ///     Type that this set contains.
  /// @tparam HashT
  ///     Hash type. Defaults to integral hashing for BWAPI usage.
  template <class T, class HashT = std::hash<int>>
  class SetContainer : public SetContainerUnderlyingT < T, HashT >
  {
  public:
#ifndef SWIG
    using SetContainerUnderlyingT<T, HashT>::SetContainerUnderlyingT;
#endif

    /// <summary>Iterates the set and erases each element x where pred(x) returns true.</summary>
    ///
    /// <param name="pred">
    ///     Predicate for removing elements.
    /// </param>
    /// @see std::erase_if
    template<class Pred>
    void erase_if(const Pred& pred) {
      auto it = this->begin();
      while (it != this->end()) {
        if (pred(*it)) it = this->erase(it);
        else ++it;
      }
    }

    /// <summary>Checks if this set contains a specific value.</summary>
    ///
    /// <param name="value">
    ///     Value to search for.
    /// </param>
    bool contains(T const &value) const
    {
      return this->count(value) != 0;
    }
  };

}
