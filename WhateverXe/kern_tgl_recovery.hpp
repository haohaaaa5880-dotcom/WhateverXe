// Exact TGL donor layout, with the native ICL single-pass recovery contract.
// The caller must qualify the binary and select host scheduler 5 first.
#ifndef kern_tgl_recovery_hpp
#define kern_tgl_recovery_hpp
#include <stdint.h>

namespace TGLRecovery {
template<typename T> inline T &field(void *p, unsigned offset) {
	return *reinterpret_cast<T *>(static_cast<uint8_t *>(p)+offset);
}

// Stamp completion for work a reset killed. IOAccelEventMachineFast2 completes
// an event when the stamp word at stampPtr[index] (event machine +0x28, filled by
// setStampBaseAddress) reaches the event's value; +0xFC + 24*index is the last
// value handed out (incrementStamp stores it, finishStamp waits for it). After a
// reset skips the hung work nothing writes those values, so finishEvent loops
// forever (waitForStamp 1 s -> restart attempt -> "no channel" -> again), inside
// process teardown and holding the accelerator lock. Linux completes a guilty
// context's requests (with an error) instead. Do the same: raise the stamp to
// its last issued value, never beyond it.
struct Completion { uint32_t before, target; bool forced; };
inline Completion completeStamp(void *eventMachine,uint32_t index) {
	Completion c {};
	if (!eventMachine) return c;
	const int32_t count=field<int32_t>(eventMachine,0x30);
	auto table=field<volatile uint32_t **>(eventMachine,0x28);
	if (count<=0 || index>=static_cast<uint32_t>(count) || !table || !table[index]) return c;
	c.before=*table[index];c.target=field<uint32_t>(eventMachine,0xFC+24*index);
	if (static_cast<int32_t>(c.target-c.before)>0) {*table[index]=c.target;c.forced=true;}
	return c;
}

struct Result {
	uintptr_t ring, context, affected;
	uint32_t index, failuresBefore, failuresAfter;
	bool resetOK;
	Completion completion;
};

// ICL resetHardwareAndReplay: reset once, account a native
// failure, resume scheduling, return to IOAccel's error/event processing.
// TGL's lower reset still owns hardware reset, context replay and CSB handling.
// Do not append a new stamp or wait inside this recovery work-loop callback.
inline Result resetAndResume(void *channel) {
	void *ring=field<void *>(channel,0x130);
	void *context=field<void *>(ring,0x20);
	void *affected=field<void *>(context,0xB8); // ICL uses B0, NOT compatible.
	Result result {reinterpret_cast<uintptr_t>(ring),reinterpret_cast<uintptr_t>(context),
		reinterpret_cast<uintptr_t>(affected),field<uint32_t>(affected,0x20),
		field<uint32_t>(affected,0x17C),0,false,{}};
	using Reset=bool (*)(void *,void *);
	result.resetOK=field<Reset>(field<void *>(ring,0),0x168)(ring,context);
	if (!result.resetOK) ++field<uint32_t>(affected,0x17C);
	result.failuresAfter=field<uint32_t>(affected,0x17C);
	// Reload after reset, as the native ICL caller does.
	void *owner=field<void *>(channel,0x18);
	void *scheduler=field<void *>(owner,0x1250); // ICL uses1280.
	// The attributed channel's outstanding events: owner+0x380 is the event
	// machine the native caller signals with this same stamp index.
	result.completion=completeStamp(field<void *>(owner,0x380),result.index);
	using Resume=void (*)(void *);
	field<Resume>(field<void *>(scheduler,0),0x180)(scheduler);
	return result;
}
}
#endif
