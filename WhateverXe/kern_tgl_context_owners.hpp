#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Tiger Lake hardware-context ownership (diagnostic only, no behavior change).
//
// IGHardwareContext::initWithOptions (donor 7B9D4) runs in the creating
// client's thread (the donor itself reads current_task there). Record which
// process created each context, keyed by the same tag the reset trace
// (kern_tgl_reset_trace.hpp) stores for every execlist slot, so a stalled
// slot can be attributed to its process.
namespace TGLContextOwners {

constexpr uint32_t contextInitEntry=0x7B9D4;
inline constexpr uint8_t contextInitPrologue[17]
	{0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC,0x28};

// Same tag as TGLResetTrace::slot(): hardware context pointer bits 4..18.
inline uint16_t contextTag(const void *context) {
	return static_cast<uint16_t>((reinterpret_cast<uintptr_t>(context)>>4)&0x7FFF);
}

constexpr unsigned nameLength=16, capacity=128;
struct Owner { uint64_t seq; uint32_t tag; int32_t pid; char name[nameLength]; };
static_assert(sizeof(Owner)==32,"serialized owner layout");
struct Ring {
	uint64_t next {};
	Owner slot[capacity] {};
};

// Lock-free: reserve a sequence, fill the slot, publish the 1-based sequence.
inline void note(Ring &ring,const void *context,int32_t pid,const char *name) {
	const uint64_t seq=__atomic_fetch_add(&ring.next,1,__ATOMIC_RELAXED);
	Owner &s=ring.slot[seq%capacity];
	__atomic_store_n(&s.seq,0,__ATOMIC_RELAXED);
	s.tag=contextTag(context);s.pid=pid;
	memset(s.name,0,sizeof(s.name));
	if (name) strncpy(s.name,name,nameLength-1);
	__atomic_store_n(&s.seq,seq+1,__ATOMIC_RELEASE);
}
}
