// CPU feature detection for kernel dispatch (codec plan §3.3).
#pragma once

#include "rcv/rcv.h"

namespace rcv {

// Highest kernel level usable on this CPU *and* OS (AVX state saved by the OS). Never RCV_ISA_AUTO.
rcv_isa detect_isa();

// SSE4.2 (crc32 instruction), used only for the optional payload CRC.
bool cpu_has_sse42();

// Resolves a requested level: AUTO -> detected; a level the machine can't run -> RCV_ERR_UNSUPPORTED.
rcv_status resolve_isa(rcv_isa requested, rcv_isa* out);

}  // namespace rcv
