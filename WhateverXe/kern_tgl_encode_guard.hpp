#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Tiger Lake hardware encode admission.
//
// The paired VA/GVA user driver builds Gen11 encode streams. On Gen12 they
// hang RCS (media-kernel walkers) or VCS (VDEnc/PAK), and native hang
// recovery cannot release the accelerator gate: WindowServer then blocks and
// the userspace watchdog panics. Hardware encode therefore stays off unless
// explicitly requested. Each guarded native token processor already fails a
// malformed stream by clearing IOAccelCommandStreamInfo+0x3c before any GPU
// submission; the guard takes exactly that native failure path. Decode,
// VPHAL, VEBox and every other token stay native.
namespace TGLEncodeGuard {

enum Path : uint32_t {EncodingCommon,MediaKernel,VDENC,AVCPAK,Count};

constexpr uint32_t streamFailedOffset=0x3c;

struct Site {
 const char *symbol;
 uint32_t entry;       // image-relative function start
 uint32_t store;       // image-relative native "movb $0,0x3c(info)" witness
 uint8_t storeLength;
 uint8_t storeBytes[5];
};

// AppleIntelTGLGraphics 16.0.0, __TEXT sha256 567511be...6d496e image.
inline constexpr Site sites[Count] {
 {"__ZN23IGAccelVideoContextMain21patch_encoding_commonER24IOAccelCommandStreamInfo17ENCODING_CMD_MODE",
  0x36B98,0x36DAB,4,{0xC6,0x46,0x3C,0x00}},
 {"__ZN23IGAccelVideoContextMain25process_token_MediaKernelER24IOAccelCommandStreamInfo",
  0x36602,0x3662A,5,{0x41,0xC6,0x46,0x3C,0x00}},
 {"__ZN24IGAccelVideoContextMedia19process_token_VDENCER24IOAccelCommandStreamInfo",
  0x3FD2E,0x40336,4,{0xC6,0x46,0x3C,0x00}},
 {"__ZN24IGAccelVideoContextMedia20process_token_AVCPAKER24IOAccelCommandStreamInfo",
  0x3E430,0x3E465,4,{0xC6,0x46,0x3C,0x00}},
};

// push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx
inline constexpr uint8_t prologue[13] {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53};

// MediaKernel copies info (RSI) into R14 before its failure store.
inline constexpr uint8_t mediaKernelInfoCopy[3] {0x49,0x89,0xF6};
constexpr uint32_t mediaKernelInfoCopyAt=0x36613;

// Qualify one site against the loaded image. solved is the absolute address
// the patcher resolved for the symbol; base/size describe the loaded image.
inline bool qualify(Path path,const uint8_t *base,size_t size,uintptr_t solved) {
 if(path>=Count || !base)return false;
 const Site &s=sites[path];
 if(size<=s.store+s.storeLength || size<=s.entry+sizeof(prologue))return false;
 if(solved!=reinterpret_cast<uintptr_t>(base)+s.entry)return false;
 if(memcmp(base+s.entry,prologue,sizeof(prologue)))return false;
 if(memcmp(base+s.store,s.storeBytes,s.storeLength))return false;
 if(path==MediaKernel) {
  if(size<=mediaKernelInfoCopyAt+sizeof(mediaKernelInfoCopy))return false;
  if(memcmp(base+mediaKernelInfoCopyAt,mediaKernelInfoCopy,sizeof(mediaKernelInfoCopy)))return false;
 }
 return true;
}

// Native failure: identical to the guarded processors' own rejection.
inline void reject(void *info) {
 if(info)reinterpret_cast<volatile uint8_t *>(info)[streamFailedOffset]=0;
}

}
