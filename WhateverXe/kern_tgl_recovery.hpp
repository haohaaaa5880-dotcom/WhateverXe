// Exact TGL donor layout, with the native ICL single-pass recovery contract.
// The caller must qualify the binary and select host scheduler 5 first.
#ifndef kern_tgl_recovery_hpp
#define kern_tgl_recovery_hpp
#include <stddef.h>
#include <stdint.h>

namespace TGLRecovery {
template<typename T> inline T &field(void *p, unsigned offset) {
	return *reinterpret_cast<T *>(static_cast<uint8_t *>(p)+offset);
}

// Native resetRegisterInfo (0x24 bytes) and IGVector<resetRegisterInfo>.
// resetGraphicsEngine (43576) reads count +0 and data +10 only. It stores
// each register's live value in +4 immediately BEFORE GDRST and writes it back
// after reset (masked: value|FFFF0000), unless getDefaultResetValue overrides.
struct ResetEntry {
	uint32_t reg, value;
	uint8_t masked;
	char name[27];
};
static_assert(sizeof(ResetEntry)==0x24,"native resetRegisterInfo layout");
struct ResetList {
	uint64_t count, capacity;
	ResetEntry *data;
};

// Render reset domain. The donor gives both IGHardwareRingBufferMain (RCS,
// type 0) and IGHardwareRingBufferCompute (CCS, type 1) GDRST mask 2. One
// GDRST therefore clears BOTH engines, but native recovery restores only the
// reset ring's own list and only replays the reset ring's streamer. Live
// capture 20261107-201357: an RCS reset left CCS without interrupts, a CCS
// reset left RCS without its restore list, and every later reset alternated.
// The native CCS list also omits RING_IMR (1A0A8), unlike the RCS list (20A8).
constexpr uint32_t renderDomainMask=2;
constexpr uint8_t renderType=0, computeType=1;
constexpr unsigned domainCapacity=64, nativeListLimit=32;

struct Extra { uint32_t reg; bool masked; };
// Render-domain state written by native IntelAccelerator::initModeRegisters
// (28E1A) and lost by GDRST mask 2. RCU_MODE (14800, masked) bit0 is the CCS
// enable: live capture run-203825 showed CCS "NOT enabled" with all ring
// registers zero after every reset until this is restored. It precedes the CCS
// list so the engine is enabled before its own registers are rewritten. The
// RCS FORCE_TO_NONPRIV slots 24D8..24FC complete the native 24D0/24D4 entries.
// RING_PSMI_CTL (RCS 2050 / CCS 1A050, masked) carries the TGL wait-for-event
// workaround that native IntelAccelerator::setAsyncSliceCount applies only at
// start (258A5/258CD: |= 0x10801080 = WAIT_FOR_EVENT_POWER_DOWN_DISABLE bit7 |
// RC_SEMA_IDLE_MSG_DISABLE bit12; Linux Wa_1607030317/1607186500/1607297627).
// GDRST mask 2 clears it and no native path reapplies it: afterwards RCS can
// power down inside MI_WAIT_FOR_EVENT and miss the display scanline event,
// so WindowServer's scanline waits never complete and every later reset
// recurs (live captures run-104625/run-110954, upstream #44/#62).
inline constexpr Extra domainExtras[] {
	{0x14800,true},{0x2050,true},
	{0x24D8,false},{0x24DC,false},{0x24E0,false},{0x24F0,false},{0x24F4,false},{0x24F8,false},{0x24FC,false}
};
// CCS engine registers at the same offsets the native RCS list restores for
// RCS (2000+): IMR A8, indirect-context WA pointer 58, FORCE_TO_NONPRIV 4D0/4D4
// and CSFE chicken D4 (masked, as in the RCS list). Values are never invented:
// the native reset samples each one immediately before GDRST.
inline constexpr Extra computeExtras[] {
	{0x1A0A8,false},{0x1A058,false},{0x1A4D0,false},{0x1A4D4,false},{0x1A0D4,true},{0x1A050,true}
};

struct Domain {
	const ResetList *native[2] {};  // donor _rcsResetRegisters, _ccsResetRegisters
	ResetList merged {};
	ResetEntry storage[domainCapacity] {};
	bool ready {false};
};

inline bool plausibleRegister(uint32_t reg) {
	return reg && !(reg&3) && reg<0x200000;
}

// Build once, after the donor's static constructors filled both native lists.
// Fails closed (ready=false, native behavior kept) on any unexpected layout.
inline bool build(Domain &domain) {
	domain.ready=false;
	uint64_t n=0;
	auto add=[&](uint32_t reg,bool masked,const char *name)->bool {
		if (!plausibleRegister(reg)) return false;
		for (uint64_t i=0;i<n;++i) if (domain.storage[i].reg==reg) return true;
		if (n>=domainCapacity) return false;
		ResetEntry &e=domain.storage[n++];
		e={};e.reg=reg;e.masked=masked;
		if (name) for (unsigned i=0;i<sizeof(e.name)-1 && name[i];++i) e.name[i]=name[i];
		return true;
	};
	// Order: RCS list, render-domain state (CCS enable), CCS list, CCS extras.
	for (unsigned l=0;l<2;++l) {
		const ResetList *list=domain.native[l];
		if (!list || !list->data || !list->count || list->count>nativeListLimit) return false;
		if (l==1) for (const auto &x:domainExtras) if (!add(x.reg,x.masked,nullptr)) return false;
		for (uint64_t i=0;i<list->count;++i) {
			const ResetEntry &e=list->data[i];
			if (!add(e.reg,e.masked!=0,e.name)) return false;
		}
	}
	for (const auto &x:computeExtras) if (!add(x.reg,x.masked,nullptr)) return false;
	domain.merged={n,n,domain.storage};
	domain.ready=true;
	return true;
}

// Execlist snapshot of one streamer (IGHardwareCommandStreamer5): active port
// index +50, port p element e at +30+p*10+e*8; a work item holds its hardware
// context at +0, stamp +18 and flags +1C (processGPUReset 3A3B9..3A4B3).
struct PortSnapshot {
	uint32_t port {0xFFFFFFFF};
	uint32_t stamp[2][2] {}, flags[2][2] {};
	bool present[2][2] {};
};

struct Result {
	uintptr_t ring, context, affected;
	uint32_t index, failuresBefore, failuresAfter;
	bool resetOK;
	// Domain handling (0xFF type = not applied).
	uint8_t type, siblingType;
	bool domainList, siblingReset;
	// Completion stamps appended after resume (native TGL caller, minus its wait).
	uint32_t siblingIndex {0xFFFFFFFF};
	uint8_t stampsAppended {};
	PortSnapshot ports[2] {};  // RCS, CCS before reset
};

using ForceWake=void (*)(void *,bool,unsigned);
using SubmitStamp=void (*)(void *);  // IGAccelFIFOChannel::submitStampCommand (4C506)

inline PortSnapshot snapshot(void *streamer) {
	PortSnapshot s;
	if (!streamer) return s;
	s.port=field<uint32_t>(streamer,0x50);
	for (unsigned p=0;p<2;++p) for (unsigned e=0;e<2;++e) {
		void *item=field<void *>(streamer,0x30+p*0x10+e*8);
		s.present[p][e]=item!=nullptr;
		if (item) {s.stamp[p][e]=field<uint32_t>(item,0x18);s.flags[p][e]=field<uint32_t>(item,0x1C);}
	}
	return s;
}

// The FIFO channel owning the first element of the streamer's active port,
// exactly the context native processGPUReset resets (3A3C0..3A3D4).
inline void *activeChannel(void *streamer) {
	if (!streamer) return nullptr;
	const uint32_t port=field<uint32_t>(streamer,0x50);
	if (port>1) return nullptr;
	void *item=field<void *>(streamer,0x30+port*0x10);
	if (!item) return nullptr;
	void *ctx=field<void *>(item,0);
	return ctx ? field<void *>(ctx,0xB8) : nullptr;
}

// Native TGL resetHardwareAndReplay (4CA79..4CA93): after resume, reserve the
// channel's next stamp (event machine owner+380, vtable 1D8) and submit a stamp
// command. The new stamp is later than every queued item the reset skipped
// (processGPUReset flag 8, evalWorkQueues 3BBF5), so their waiters complete.
// Only the native sleepForStamp loop that followed is omitted: it deadlocks
// on Tahoe and refilled the 511-slot queue on repeated failure.
inline bool appendStamp(void *owner,void *channel,SubmitStamp submitStamp) {
	if (!owner || !channel || !submitStamp) return false;
	void *events=field<void *>(owner,0x380);
	if (!events) return false;
	using Increment=void (*)(void *,uint32_t);
	field<Increment>(field<void *>(events,0),0x1D8)(events,field<uint32_t>(channel,0x20));
	submitStamp(channel);
	return true;
}

// True when native resetGraphicsEngine will take its GDRST branch (43716):
// the per-type engine record flags 4/8 select a non-GDRST soft path instead.
inline bool usesGDRST(void *ring,uint8_t type) {
	void *record=field<void *>(ring,0x8B8+type*8u);
	return record && !(field<uint32_t>(record,0xA4)&0xC);
}

// ICL resetHardwareAndReplay: reset once, account a native
// failure, resume scheduling, return to IOAccel's error/event processing.
// TGL's lower reset still owns hardware reset, context replay and CSB handling.
// Never WAIT inside this recovery work-loop callback (native sleepForStamp
// deadlocks on Tahoe). With submitStamp, one completion stamp per affected
// channel is appended after resume, as the native TGL caller does: without it
// items the reset skipped are never completed and their waiters (including
// command-queue teardown) block forever (live capture run-075219).
// With a ready domain, a render-domain GDRST restores both engines' registers
// and the sibling engine receives the same native processGPUReset the reset
// engine already received inside resetGraphicsEngine.
inline Result resetAndResume(void *channel,Domain *domain=nullptr,ForceWake forceWake=nullptr,
	SubmitStamp submitStamp=nullptr) {
	void *ring=field<void *>(channel,0x130);
	void *context=field<void *>(ring,0x20);
	void *affected=field<void *>(context,0xB8); // ICL uses B0, NOT compatible.
	Result result {reinterpret_cast<uintptr_t>(ring),reinterpret_cast<uintptr_t>(context),
		reinterpret_cast<uintptr_t>(affected),field<uint32_t>(affected,0x20),
		field<uint32_t>(affected,0x17C),0,false,0xFF,0xFF,false,false};
	const uint8_t type=field<uint8_t>(ring,0x40);
	if (submitStamp) {
		void *before=field<void *>(field<void *>(channel,0x18),0x1250);
		for (unsigned t=0;t<2;++t) result.ports[t]=snapshot(field<void *>(before,0xCA0+t*8u));
	}
	const bool shared=domain && domain->ready && forceWake &&
		field<uint32_t>(ring,0x70)==renderDomainMask &&
		(type==renderType || type==computeType) && usesGDRST(ring,type);
	void *nativeList=field<void *>(ring,0x78);
	if (shared) {
		result.type=type;
		field<void *>(ring,0x78)=&domain->merged;
		result.domainList=true;
	}
	using Reset=bool (*)(void *,void *);
	result.resetOK=field<Reset>(field<void *>(ring,0),0x168)(ring,context);
	if (shared) field<void *>(ring,0x78)=nativeList;
	if (!result.resetOK) ++field<uint32_t>(affected,0x17C);
	result.failuresAfter=field<uint32_t>(affected,0x17C);
	// Reload after reset, as the native ICL caller does.
	void *owner=field<void *>(channel,0x18);
	void *scheduler=field<void *>(owner,0x1250); // ICL uses1280.
	void *siblingChannel=nullptr;
	if (shared && result.resetOK) {
		const uint8_t sibling=type==renderType ? computeType : renderType;
		result.siblingType=sibling;
		// IGScheduler5::processGPUReset (380D8) dereferences streamer CA0+type*8.
		if (void *streamer=field<void *>(scheduler,0xCA0+sibling*8u)) {
			// Read the context it is about to reset before processGPUReset runs.
			siblingChannel=activeChannel(streamer);
			if (siblingChannel==affected) siblingChannel=nullptr;
			if (siblingChannel) result.siblingIndex=field<uint32_t>(siblingChannel,0x20);
			void *accelerator=field<void *>(ring,0x10);
			forceWake(accelerator,true,7); // native holds this across processGPUReset
			using ProcessReset=void (*)(void *,uint32_t,void *);
			field<ProcessReset>(field<void *>(scheduler,0),0x190)(scheduler,sibling,nullptr);
			forceWake(accelerator,false,7);
			result.siblingReset=true;
		}
	}
	using Resume=void (*)(void *);
	field<Resume>(field<void *>(scheduler,0),0x180)(scheduler);
	// Native order: resume, then the completion stamp (no sleepForStamp).
	if (submitStamp) {
		result.stampsAppended+=appendStamp(owner,affected,submitStamp);
		result.stampsAppended+=appendStamp(owner,siblingChannel,submitStamp);
	}
	return result;
}
}
#endif
