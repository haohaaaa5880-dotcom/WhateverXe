// Exact TGL donor layout, with the native ICL single-pass recovery contract.
// The caller must qualify the binary and select host scheduler 5 first.
#ifndef kern_tgl_recovery_hpp
#define kern_tgl_recovery_hpp
#include <stdint.h>

namespace TGLRecovery {
template<typename T> inline T &field(void *p, unsigned offset) {
	return *reinterpret_cast<T *>(static_cast<uint8_t *>(p)+offset);
}

// Cross-engine release. The native reset skips the attributed engine's hung
// element (IGHardwareCommandStreamer5::processGPUReset resubmits only its stamp),
// but another engine can be polling an MI_SEMAPHORE_WAIT for a value that the
// skipped work would have written. Nothing resets that engine: every later hang
// is attributed to whichever context queues behind it, the attributed (idle)
// engine is reset again, and the waiter never moves (observed: BCS IPEHR
// 0x0E009002 for 10+ minutes after one RCS reset). Linux completes the guilty
// request so waiters are released; here, give the waiting engine the same native
// reset, which skips its stalled element the same way.
struct Engine { uintptr_t vtable; uint32_t mmio; }; // ring vtable (ZTV+0x10), engine MMIO base
struct Probe {
	Engine engines[2]; // e.g. IGHardwareRingBufferBlit -> 0x22000, IGHardwareRingBufferMain -> 0x2000
	uint32_t (*read)(void *mmio,uint32_t offset);
	void (*delay)(unsigned microseconds);
};
// Release record: engines seen with work in flight, semaphore waits seen,
// of those stalled (ACTHD and IPEHR unchanged over the delay), resets issued,
// resets reported OK, last IPEHR before / after.
struct Release { uint32_t busy, semaphore, stalled, resets, resetOK, ipehr, ipehrAfter; };

// Guilty-stamp completion. IOAccelEventMachineFast2 completes an event when the
// stamp word at stampPtr[index] (event machine +0x28, filled by
// setStampBaseAddress) reaches the event's value; +0xFC + 24*index is the last
// value handed out (incrementStamp stores it, finishStamp waits for it). After a
// reset skips the hung work, nothing writes the guilty channel's outstanding
// values, so finishEvent loops forever (waitForStamp 1 s -> restart attempt ->
// "no channel" -> again) - inside process teardown, holding the accelerator lock.
// Linux completes a guilty context's requests (with an error) instead. Do the
// same: raise the guilty stamp to its last issued value, never beyond it.
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
	Release release;
	Completion completion;
};

constexpr uint32_t ringIPEHR=0x68,ringACTHD=0x74,engineSlots=6;
inline bool semaphoreWait(uint32_t header) {return (header>>23)==0x1C;} // MI_SEMAPHORE_WAIT

using Reset=bool (*)(void *,void *);
inline Release releaseWaiters(void *scheduler,void *attributed,const Probe &probe) {
	Release r {};
	void *accelerator=field<void *>(scheduler,0x10);
	if (!accelerator || !probe.read || !probe.delay) return r;
	const uint64_t mask=field<uint64_t>(accelerator,0x1300);
	for (unsigned i=0;i<engineSlots;++i) {
		if (!(mask&(1ULL<<i))) continue;
		void *streamer=field<void *>(scheduler,0xCA0+8*i);
		if (!streamer) continue;
		const uint32_t port=field<uint32_t>(streamer,0x50);
		if (port>1) continue;
		void *element=field<void *>(streamer,0x30+16*port);
		void *context=element ? field<void *>(element,0) : nullptr;
		void *channel=context ? field<void *>(context,0xB8) : nullptr;
		void *ring=channel ? field<void *>(channel,0x130) : nullptr;
		if (!ring || ring==attributed) continue;
		const Engine *engine=nullptr;
		for (auto &e:probe.engines) if (e.vtable && field<uintptr_t>(ring,0)==e.vtable) engine=&e;
		void *mmio=field<void *>(ring,0x50);
		if (!engine || !mmio) continue;
		++r.busy;
		const uint32_t ipehr=probe.read(mmio,engine->mmio+ringIPEHR);r.ipehr=ipehr;
		if (!semaphoreWait(ipehr)) continue;
		++r.semaphore;
		const uint32_t acthd=probe.read(mmio,engine->mmio+ringACTHD);
		probe.delay(1000);
		if (probe.read(mmio,engine->mmio+ringACTHD)!=acthd || probe.read(mmio,engine->mmio+ringIPEHR)!=ipehr) continue;
		++r.stalled;++r.resets;
		if (field<Reset>(field<void *>(ring,0),0x168)(ring,field<void *>(ring,0x20))) ++r.resetOK;
		r.ipehrAfter=probe.read(mmio,engine->mmio+ringIPEHR);
	}
	return r;
}

// ICL resetHardwareAndReplay: reset once, account a native
// failure, resume scheduling, return to IOAccel's error/event processing.
// TGL's lower reset still owns hardware reset, context replay and CSB handling.
// Do not append a new stamp or wait inside this recovery work-loop callback.
inline Result resetAndResume(void *channel,const Probe *probe=nullptr) {
	void *ring=field<void *>(channel,0x130);
	void *context=field<void *>(ring,0x20);
	void *affected=field<void *>(context,0xB8); // ICL uses B0, NOT compatible.
	Result result {reinterpret_cast<uintptr_t>(ring),reinterpret_cast<uintptr_t>(context),
		reinterpret_cast<uintptr_t>(affected),field<uint32_t>(affected,0x20),
		field<uint32_t>(affected,0x17C),0,false,{},{}};
	result.resetOK=field<Reset>(field<void *>(ring,0),0x168)(ring,context);
	if (!result.resetOK) ++field<uint32_t>(affected,0x17C);
	result.failuresAfter=field<uint32_t>(affected,0x17C);
	// Reload after reset, as the native ICL caller does.
	void *owner=field<void *>(channel,0x18);
	void *scheduler=field<void *>(owner,0x1250); // ICL uses1280.
	if (probe) result.release=releaseWaiters(scheduler,ring,*probe);
	// Guilty channel's outstanding events: owner+0x380 is the event machine the
	// native caller signals with this same stamp index.
	result.completion=completeStamp(field<void *>(owner,0x380),result.index);
	using Resume=void (*)(void *);
	field<Resume>(field<void *>(scheduler,0),0x180)(scheduler);
	return result;
}
}
#endif
