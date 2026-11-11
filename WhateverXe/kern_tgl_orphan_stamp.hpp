#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Tiger Lake orphaned-stamp retirement.
//
// After a render-domain GPU reset, work that other clients had in flight on
// the reset engines is gone, but their stamp indices still read
// submitted > returned. IOAccelEventMachineFast2::checkGPUProgress then
// declares a hang on every such index every few seconds, and
// IOAccelEventMachine2::restart_channel asks the donor's
// IGAccelEventMachine::eventTimeout (donor 15F22) for the channel to restart.
// The donor halts the command streamer, finds no active and no pending
// context (IGTimeoutReason 4), resumes it and returns no channel, so
// restart_channel logs "GPURestartSkipped / no channel associated" and the
// stamp stays stuck. Every flag makes waiters fail ("a graphics error
// occurred") and every waiter on the dead stamp sleeps up to 5 s: the
// minutes of stutter seen after each reset.
//
// Nothing can complete such a stamp: the engine owning it has no context
// loaded or queued. Retire it the way the family itself retires every stamp
// when the device terminates (IOAccelEventMachineFast2::deviceTerminatedUnlocked:
// *stampAddress[i] = submitted[i]; sfence; signalAllStamps), for that one
// index only, and only when the donor itself classified it as reason 4
// during a restart of exactly that index.
namespace TGLOrphanStamp {

// Donor (AppleIntelTGLGraphics 16.0.0) entry points.
constexpr uint32_t eventTimeoutEntry=0x15F22, encodeDebugInfoEntry=0x2E460;
inline constexpr uint8_t eventTimeoutPrologue[23] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC,0x18,
	0x41,0x89,0xF5,0x49,0x89,0xFE};
inline constexpr uint8_t encodeDebugInfoPrologue[26] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xEC,0x08,0x04,0x00,0x00,
	0x41,0x89,0xF7,0x49,0x89,0xFE};
constexpr uint32_t reasonNoContext=4;  // IGTimeoutReason: no active/pending context on the engine

// IOAcceleratorFamily2 event machine layout, checked at run time against the
// code behind the object's own vtable before any write.
constexpr size_t stampAddresses=0x28;   // uint32_t volatile *[numStamps]
constexpr size_t numStamps=0x30;        // int32_t
constexpr size_t progressBase=0xF0, progressStride=0x18;
constexpr size_t submittedField=0x14;   // within a progress entry (0x104 + i*0x18)
constexpr size_t restartIndex=0x90, restartType=0x94;  // set by signalHardwareError/hardwareErrorEvent
constexpr int32_t restartTypeChannel=2;
constexpr size_t slotCheckGPUProgress=0x210/8, slotSignalStamp=0x228/8, slotDeviceTerminated=0x250/8;

// IOAccelEventMachineFast2::checkGPUProgress, first 0x58 bytes: reads
// numStamps (0x30), progress entries at 0xF0, stamp addresses at 0x28 and
// submitted at entry+0x14.
inline constexpr uint8_t checkGPUProgressCode[0x58] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC,
	0x18,0x48,0xC7,0x45,0xD0,0x00,0x00,0x00,0x00,0x83,0x7F,0x30,0x00,0x0F,0x8E,0x27,
	0x01,0x00,0x00,0x48,0x89,0xFB,0x48,0xB8,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,
	0x4C,0x8D,0x75,0xC8,0x49,0x89,0x06,0x4C,0x8D,0xAF,0xF0,0x00,0x00,0x00,0x45,0x31,
	0xE4,0x48,0x8B,0x43,0x28,0x4A,0x8B,0x04,0xE0,0x44,0x8B,0x38,0x41,0x8B,0x45,0x14,
	0x44,0x29,0xF8,0x85,0xC0,0x0F,0x8E,0xAF};
// IOAccelEventMachine2::signalStamp(int,unsigned), up to its first call:
// wakes the waiters sleeping on stampAddresses[index].
inline constexpr uint8_t signalStampCode[0x1F] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x53,0x50,0x41,0x89,0xD7,0x49,0x89,0xFE,
	0x48,0x8B,0x47,0x28,0x48,0x63,0xDE,0x48,0x8B,0x3C,0xD8,0x31,0xF6,0x31,0xD2};
// IOAccelEventMachineFast2::deviceTerminatedUnlocked, the retirement loop
// this module repeats for one index.
inline constexpr uint8_t deviceTerminatedCode[0x40] {
	0x55,0x48,0x89,0xE5,0x48,0x8B,0x47,0x28,0x48,0x85,0xC0,0x74,0x30,0x8B,0x4F,0x30,
	0x85,0xC9,0x7E,0x29,0x48,0x8D,0x97,0x04,0x01,0x00,0x00,0x31,0xF6,0x4C,0x8B,0x04,
	0xF0,0x4D,0x85,0xC0,0x74,0x08,0x8B,0x0A,0x41,0x89,0x08,0x8B,0x4F,0x30,0x48,0xFF,
	0xC6,0x4C,0x63,0xC1,0x48,0x83,0xC2,0x18,0x4C,0x39,0xC6,0x7C,0xE0,0x0F,0xAE,0xF8};

inline bool layoutKnown(const void *checkGPUProgress,const void *signalStamp,const void *deviceTerminated) {
	return checkGPUProgress && signalStamp && deviceTerminated &&
		!memcmp(checkGPUProgress,checkGPUProgressCode,sizeof(checkGPUProgressCode)) &&
		!memcmp(signalStamp,signalStampCode,sizeof(signalStampCode)) &&
		!memcmp(deviceTerminated,deviceTerminatedCode,sizeof(deviceTerminatedCode));
}

template<typename T> inline T field(const void *object,size_t offset) {
	T v;memcpy(&v,static_cast<const uint8_t *>(object)+offset,sizeof(v));return v;
}

// Called for exactly the index restart_channel is working on.
inline bool restartingIndex(const void *machine,int index) {
	return field<int32_t>(machine,restartType)==restartTypeChannel &&
		field<int32_t>(machine,restartIndex)==index;
}

enum class Outcome : uint32_t { NotRestart, Channel, OtherReason, Layout, Range, NotBusy, Retired };

struct Result { Outcome outcome; uint32_t submitted, returned; };

// Decide and, for Retired, write the submitted value into the stamp. The
// caller then wakes the waiters through the machine's own signalStamp.
inline Result retire(void *machine,int index,const void *channel,uint32_t reason,bool layout) {
	if (!restartingIndex(machine,index)) return {Outcome::NotRestart,0,0};
	if (channel) return {Outcome::Channel,0,0};
	if (reason!=reasonNoContext) return {Outcome::OtherReason,0,0};
	if (!layout) return {Outcome::Layout,0,0};
	const int32_t count=field<int32_t>(machine,numStamps);
	auto stamps=field<uint32_t volatile **>(machine,stampAddresses);
	if (index<0 || index>=count || count>256 || !stamps || !stamps[index]) return {Outcome::Range,0,0};
	const uint32_t submitted=field<uint32_t>(machine,progressBase+static_cast<size_t>(index)*progressStride+submittedField);
	const uint32_t returned=*stamps[index];
	if (static_cast<int32_t>(submitted-returned)<=0) return {Outcome::NotBusy,submitted,returned};
	*stamps[index]=submitted;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return {Outcome::Retired,submitted,returned};
}

// Registry record TGLOrphanStampLive (uint32_t): schema, then one counter per
// Outcome, then the last retired index and how many stamps it skipped.
constexpr unsigned outcomes=7, recordWords=1+outcomes+2;

}  // namespace TGLOrphanStamp
