#pragma once
#include <cstddef>
#include <map>
#include <list>

#include <BWAPI/InterfaceEvent.h>

namespace BWAPI
{
  /// <summary>This generalized class allows the application of features that are common to all interface
  /// classes.</summary>
  ///
  /// @tparam T
  ///     The class that inherits this interface.
  template < typename T >
  class Interface
  {
  protected:
    /// @cond HIDDEN
    Interface() = default;
    virtual ~Interface() {};

    std::map<int,void*> clientInfo;
    std::list< InterfaceEvent<T> > interfaceEvents;

    friend class GameImpl;

    // ---- interface-event live count (sb-perf) --------------------------------------------
    // Almost no bot ever calls registerEvent, but GameImpl::processInterfaceEvents() walks
    // every accessible unit, bullet, region and player once per frame regardless, purely to
    // find an empty interfaceEvents list. By then those objects have been evicted, so that
    // walk is close to pure L2/LL miss cost for zero effect (measured: the single largest
    // remaining per-unit pass in the dual-host cache profile).
    //
    // liveEventCount is the number of InterfaceEvent<T> objects alive across ALL instances of
    // T. When it is zero, every list is provably empty, so updateEvents() is a no-op and
    // interfaceEvents.clear() is a no-op -- the whole walk can be skipped with no observable
    // difference. This is not a heuristic: rule 9 holds by construction, not by measurement.
    //
    // The accounting is deliberately allowed to drift HIGH, never low. Every element was
    // counted on push_back, and every decrement is bounded by what a list actually holds, so
    // total decrements can never exceed total increments. A missed decrement costs us the
    // optimisation; it can never cause a wrongly skipped walk. Any .clear() that bypasses
    // clearEvents() (e.g. the BWAPIClient copy) is therefore safe, just pessimal.
    //
    // COMPATIBILITY BOUNDARY, stated plainly. Source compatibility is exact: registerEvent's
    // signature and semantics are unchanged, so any bot RECOMPILED against these headers behaves
    // identically with the skip on. Binary layout is unchanged too (the counter is static; the
    // new members are non-virtual), so existing .so files still load and run. The one hole is a
    // bot binary compiled against UPSTREAM headers that calls registerEvent on a Unit / Bullet /
    // Region / Force / Player: it inlined the old body, which does not increment, so BWAPI would
    // skip servicing its events. Such a binary must run with SB_INTERFACE_EVENT_SKIP=0. Nothing
    // in this tree is in that category -- the only registerEvent caller is ExampleAIModule, and
    // it registers on Game, whose walk is deliberately left UNCONDITIONAL for exactly this
    // reason. This hole cannot be closed by counting harder: catching it would mean observing
    // every list, which is the cost being removed.
    static std::size_t liveEventCount;

    // Function manages events and updates it for the given frame
    void updateEvents()
    {
      auto e = interfaceEvents.begin();
      while ( e != interfaceEvents.end() )
      {
        if ( e->isFinished() )
        {
          e = interfaceEvents.erase(e);
          --liveEventCount;
        }
        else
        {
          e->execute(static_cast<T*>(this));
          ++e;
        }
      }
    };

    // Counted equivalent of interfaceEvents.clear(); use this instead so liveEventCount stays
    // tight. Correctness does not depend on it (see liveEventCount).
    void clearEvents()
    {
      liveEventCount -= interfaceEvents.size();
      interfaceEvents.clear();
    };

    // For SB_INTERFACE_EVENT_SKIP=verify: the invariant the skip rests on.
    bool eventListEmpty() const
    {
      return interfaceEvents.empty();
    };
    /// @endcond
  public:
    /// @cond HIDDEN
    // True if any instance of T holds a registered event; false means the per-object event
    // walk for T is provably a no-op this frame.
    static bool anyInterfaceEvents()
    {
      return liveEventCount != 0;
    };
    /// @endcond
    /// <summary>Retrieves a pointer or value at an index that was stored for this interface using
    /// setClientInfo.</summary>
    ///
    /// <param name="key">
    ///   The key containing the value to retrieve. Default is 0.
    /// </param>
    /// 
    /// @retval nullptr if index is out of bounds.
    /// @returns The client info at the given index.
    /// @see setClientInfo
    void *getClientInfo(int key = 0) const
    {
      // Retrieve iterator to element at index
      auto result = this->clientInfo.find(key);

      // Return a default value if not found
      if ( result == this->clientInfo.end() )
        return nullptr;

      // return the desired value
      return result->second;
    };

    template <typename CT>
    CT getClientInfo(int key = 0) const
    {
      return (CT)(long)this->getClientInfo(key);
    };

    /// <summary>Associates one or more pointers or values with any BWAPI interface.</summary>
    ///
    /// This client information is managed entirely by the AI module. It is not modified by BWAPI.
    /// @warning If a pointer to allocated memory is used, then the AI module is responsible for
    /// deallocating the memory when the game ends.
    ///
    /// If client info at the given index has already been set, then it will be overwritten.
    ///
    /// <param name="clientInfo">
    ///   The data to associate with this interface.
    /// </param>
    /// <param name="key">
    ///   The key to use for this data. Default is 0.
    /// </param>
    ///
    /// @see getClientInfo
    template < typename V >
    void setClientInfo(const V &clientInfo, int key = 0)
    {
      this->clientInfo[key] = (void*)clientInfo;
    };

    /// <summary>Registers an event and associates it with the current Interface object.</summary>
    /// Events can be used to automate tasks (like train X @Marines until Y of them have been
    /// created by the given @Barracks) or to create user-defined callbacks.
    ///
    /// <param name="action">
    ///   The action callback to be executed when the event conditions are true. It is of type
    ///   void fn(T *inst) where fn is the function name and inst is a pointer to the instance of
    ///   the object performing the action.
    /// </param>
    /// <param name="condition"> (optional)
    ///   The condition callback which will return true if the action is intended to be executed.
    ///   It is of type bool fn(T *inst) where fn is the function name and inst is a pointer to the
    ///   instance of the object performing the check. The condition will always be true if omitted.
    /// </param>
    /// <param name="timesToRun"> (optional)
    ///   The number of times to execute the action before the event is removed. If the value is
    ///   negative, then the event will never be removed. The value will be -1 if omitted, causing
    ///   the event to execute until the game ends.
    /// </param>
    /// <param name="framesToCheck"> (optional)
    ///   The number of frames to skip between checks. If this value is 0, then a condition check is
    ///   made once per frame. If this value is 1, then the condition for this event is only checked
    ///   every other frame. This value is 0 by default, meaning the event's condition is checked
    ///   every frame.
    /// </param>
    void registerEvent(const std::function<void(T*)> &action, const std::function<bool(T*)> &condition = nullptr, int timesToRun = -1, int framesToCheck = 0)
    {
      interfaceEvents.push_back( InterfaceEvent<T>(action,condition,timesToRun,framesToCheck) );
      ++liveEventCount;
    };
  };

  template < typename T >
  std::size_t Interface<T>::liveEventCount = 0;


}
