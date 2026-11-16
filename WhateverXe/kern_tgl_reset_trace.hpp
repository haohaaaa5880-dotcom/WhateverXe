#pragma once
#include <stddef.h>
#include <stdint.h>

// Tiger Lake render-domain execlist trace (diagnostic only, never alters state).
//
// After a render-domain reset the native scheduler can keep a phantom in-flight
// element: RCS idles with head==tail while newer work stays queued until the
// next hang check resets again (live capture run-100826, ~14.4 s cadence). The
// existing tglHost histories only cover the first events after boot. This ring
// keeps the most recent CSB intake, submission and reset transitions for RCS
// and CCS, with all four execlist slots, so the missed transition is visible.
//
// Lock-free: CSB intake can run in interrupt context. A writer reserves a
// sequence number atomically, fills the slot, then publishes the sequence.
namespace TGLResetTrace {

enum Kind : uint8_t {
	CSBBefore=1, CSBAfter=2, SubmitBefore=3, SubmitAfter=4,
	ResetBefore=5, ResetAfter=6, Recovery=7
};

constexpr unsigned capacity=512, words=8;

struct Ring {
	uint64_t next {};                 // next sequence to reserve
	uint64_t record[capacity][words] {};
};

template<typename T> inline T load(const void *p,size_t offset) {
	return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p)+offset);
}

// One execlist slot: present<<63 | context tag<<48 | flags(16)<<32 | stamp.
// Work item: hardware context +0, stamp +18, flags +1C (processGPUReset).
inline uint64_t slot(const void *item) {
	if (!item) return 0;
	const uintptr_t ctx=load<uintptr_t>(item,0);
	return (1ULL<<63) | (static_cast<uint64_t>((ctx>>4)&0x7FFF)<<48) |
		(static_cast<uint64_t>(load<uint32_t>(item,0x1C)&0xFFFF)<<32) | load<uint32_t>(item,0x18);
}

// Streamer (IGHardwareCommandStreamer5): active port +50, slots +30..+48,
// CSB read head +1C04. Engine id is the descriptor's first byte (+28 -> [0]).
inline bool engineOf(const void *streamer,uint8_t &engine) {
	if (!streamer) return false;
	const uint8_t *descriptor=load<const uint8_t *>(streamer,0x28);
	if (!descriptor) return false;
	engine=descriptor[0];
	return engine<=1; // RCS, CCS: the shared render reset domain only
}

inline void record(Ring &ring,uint64_t now,Kind kind,const void *streamer,uint32_t aux,uint64_t raw) {
	uint8_t engine=0;
	if (!engineOf(streamer,engine)) return;
	const uint64_t seq=__atomic_fetch_add(&ring.next,1,__ATOMIC_RELAXED);
	uint64_t *q=ring.record[seq%capacity];
	__atomic_store_n(&q[0],0,__ATOMIC_RELAXED); // invalid while being written
	q[1]=now;
	q[2]=kind | static_cast<uint64_t>(engine)<<8 |
		static_cast<uint64_t>(load<uint32_t>(streamer,0x50)&0xFF)<<16 |
		static_cast<uint64_t>(load<uint32_t>(streamer,0x1C04)&0xFF)<<24 |
		static_cast<uint64_t>(aux)<<32;
	q[3]=raw;
	for (unsigned i=0;i<4;++i) q[4+i]=slot(load<const void *>(streamer,0x30+i*8));
	__atomic_store_n(&q[0],seq+1,__ATOMIC_RELEASE); // valid: 1-based sequence
}
}
