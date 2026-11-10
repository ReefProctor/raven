#pragma once

#include "objects.h"

#include <vector>

namespace raven {

std::optional<uint32_t> event_access(uint32_t requested);

class EventObject : public KernelObject {
public:
	EventObject(uint32_t type, bool signaled) : type(type), local_signaled(signaled) {}
	explicit EventObject(int descriptor);
	~EventObject() override;
	int signaled(uint32_t thread) const override;
	void consume(uint32_t thread) override;
	bool try_consume(uint32_t thread) override;
	int set_state(bool signaled) override;
	bool describe(InheritedHandle &record) const override;
	std::u16string_view type_name() const override { return u"Event"; }
	bool manual_reset() const { return type == 0; }
	// A pulse for one waiting thread: the event looks signaled to that thread alone until its wait takes it or the pulse is revoked,
	// so a waiter that the scheduler looks at later is still released and nobody else ever sees the event set.
	void grant_pulse(uint32_t thread);
	void revoke_pulse(uint32_t thread);
	// Releases the host threads blocked on the event the way a pulse does, without changing its state.
	void pulse_host_waiters();

private:
	friend class NativeObjects;
	struct Shared;
	struct Waiter { bool ready = false; };
	mutable std::mutex mutex;
	mutable std::condition_variable changed;
	uint32_t type;
	bool local_signaled;
	std::deque<std::shared_ptr<Waiter>> waiters;
	std::vector<uint32_t> pulses;
	mutable int descriptor = -1;
	mutable Shared *shared = nullptr;
	bool is_set() const;
	bool take();
};

}
