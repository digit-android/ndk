//===--------------------------- Unwind-arm.c ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//
//  Implements ARM zero-cost C++ exceptions
//
//===----------------------------------------------------------------------===//

#include <unwind.h>

#include <stdbool.h>
#include <stdlib.h>

#include "config.h"
#include "libunwind.h"
#include "../private_typeinfo.h"

#if __arm__ && !CXXABI_SJLJ
namespace {

// Strange order: take words in order, but inside word, take from most to least
// signinficant byte.
uint8_t getByte(uint32_t* data, size_t offset) {
  uint8_t* byteData = reinterpret_cast<uint8_t*>(data);
  return byteData[(offset & ~0x03) + (3 - (offset&0x03))];
}

const char* getNextWord(const char* data, uint32_t* out) {
  *out = *reinterpret_cast<const uint32_t*>(data);
  return data + 4;
}

const char* getNextNibble(const char* data, uint32_t* out) {
  *out = *reinterpret_cast<const uint16_t*>(data);
  return data + 2;
}

static inline uint32_t signExtendPrel31(uint32_t data) {
  return data | ((data & 0x40000000u) << 1);
}

struct Descriptor {
   // See # 9.2
   typedef enum {
     SU16 = 0, // Short descriptor, 16-bit entries
     LU16 = 1, // Long descriptor,  16-bit entries
     LU32 = 3, // Long descriptor,  32-bit entries
     RESERVED0 =  4, RESERVED1 =  5, RESERVED2  = 6,  RESERVED3  =  7,
     RESERVED4 =  8, RESERVED5 =  9, RESERVED6  = 10, RESERVED7  = 11,
     RESERVED8 = 12, RESERVED9 = 13, RESERVED10 = 14, RESERVED11 = 15
   } Format;

   // See # 9.2
   typedef enum {
     CLEANUP = 0x0,
     FUNC    = 0x1,
     CATCH   = 0x2,
     INVALID = 0x4
   } Kind;
};

_Unwind_Reason_Code ProcessDescriptors(
    _Unwind_State state,
    _Unwind_Control_Block* ucbp,
    struct _Unwind_Context* context,
    Descriptor::Format format,
    const char* descriptorStart,
    int flags) {
  // EHT is inlined in the index using compact form. No descriptors. #5
  if (flags & 0x1)
    return _URC_CONTINUE_UNWIND;

  const char* descriptor = descriptorStart;
  uint32_t descriptorWord;
  getNextWord(descriptor, &descriptorWord);
  while (descriptorWord) {
    // Read descriptor based on # 9.2.
    uint32_t length;
    uint32_t offset;
    switch (format) {
      case Descriptor::LU32:
        descriptor = getNextWord(descriptor, &length);
        descriptor = getNextWord(descriptor, &offset);
      case Descriptor::LU16:
        descriptor = getNextNibble(descriptor, &length);
        descriptor = getNextNibble(descriptor, &offset);
      default:
        assert(false);
        return _URC_FAILURE;
    }

    // See # 9.2 table for decoding the kind of descriptor. It's a 2-bit value.
    Descriptor::Kind kind = static_cast<Descriptor::Kind>((length & 0x1) | ((offset & 0x1) << 1));

    // Clear off flag from last bit.
    length &= ~1;
    offset &= ~1;
    uintptr_t scopeStart = ucbp->pr_cache.fnstart + offset;
    uintptr_t scopeEnd = scopeStart + length;
    uintptr_t pc = _Unwind_GetIP(context);
    bool isInScope = (scopeStart <= pc) && (pc < scopeEnd);

    switch (kind) {
      case Descriptor::CLEANUP: {
        // TODO(ajwong): Handle cleanup descriptors.
        break;
      }
      case Descriptor::FUNC: {
        // TODO(ajwong): Handle function descriptors.
        break;
      }
      case Descriptor::CATCH: {
        // Catch descriptors require gobbling one more word.
        uint32_t landing_pad;
        descriptor = getNextWord(descriptor, &landing_pad);

        if (isInScope) {
          // TODO(ajwong): This is only phase1 compatible logic. Implement
          // phase2.
          bool is_reference_type = landing_pad & 0x80000000;
          landing_pad = signExtendPrel31(landing_pad & ~0x80000000);
          if (landing_pad == 0xffffffff) {
            return _URC_HANDLER_FOUND;
          } else if (landing_pad == 0xfffffffe ) {
            return _URC_FAILURE;
          } else {
            void* matched_object;
            /*
            if (__cxxabiv1::__cxa_type_match(ucbp,
                                             reinterpret_cast<const std::type_info*>(landing_pad),
                                             is_reference_type, &matched_object) != __cxxabiv1::ctm_failed)
                return _URC_HANDLER_FOUND;
                */
            _LIBUNWIND_ABORT("Type matching not implemented");
          }
        }
        break;
      }
      default:
        _LIBUNWIND_ABORT("Invalid descriptor kind found.");
    };

    getNextWord(descriptor, &descriptorWord);
  }

  return _URC_CONTINUE_UNWIND;
}

_Unwind_Reason_Code unwindOneFrame(
    _Unwind_State state,
    _Unwind_Control_Block* ucbp,
    struct _Unwind_Context* context) {
  // TODO(piman): handle phase1/phase2.

  // Read the compact model EHT entry's header # 6.3
  uint32_t* unwindingData = ucbp->pr_cache.ehtp;
  uint32_t unwindInfo = *unwindingData;
  assert((unwindInfo & 0xf0000000) == 0x80000000 && "Must be a compact entry");
  Descriptor::Format format = static_cast<Descriptor::Format>((unwindInfo & 0x0f000000) >> 24);
  size_t len = 0;
  size_t startOffset = 0;
  switch (format) {
    case Descriptor::SU16:
      len = 4;
      startOffset = 1;
      break;
    case Descriptor::LU16:
    case Descriptor::LU32:
      len = 4 + 4 * ((unwindInfo & 0x00ff0000) >> 16);
      startOffset = 2;
      break;
    default:
      return _URC_FAILURE;
  }

  // Handle descriptors before unwinding so they are processed in the context
  // of the correct stack frame.
  _Unwind_Reason_Code result =
      ProcessDescriptors(
          state, ucbp, context, format,
          reinterpret_cast<const char*>(ucbp->pr_cache.ehtp) + len,
          ucbp->pr_cache.additional);

  if (result != _URC_CONTINUE_UNWIND)
    return result;

  return _Unwind_VRS_Interpret(context, unwindingData, startOffset, len);
}

} // end anonymous namespace

extern "C" _Unwind_Reason_Code _Unwind_VRS_Interpret(
    _Unwind_Context* context,
    uint32_t* data,
    size_t offset,
    size_t len) {
  bool wrotePC = false;
  bool finish = false;
  while (offset < len && !finish) {
    uint8_t byte = getByte(data, offset++);
    if ((byte & 0x80) == 0) {
      uint32_t sp;
      _Unwind_VRS_Get(context, _UVRSC_CORE, UNW_ARM_SP, _UVRSD_UINT32, &sp);
      if (byte & 0x40)
        sp -= ((byte & 0x3f) << 2) + 4;
      else
        sp += (byte << 2) + 4;
      _Unwind_VRS_Set(context, _UVRSC_CORE, UNW_ARM_SP, _UVRSD_UINT32, &sp);
    } else {
      switch (byte & 0xf0) {
        case 0x80: {
          if (offset >= len)
            return _URC_FAILURE;
          uint16_t registers = ((byte & 0x0f) << 12) | (getByte(data, offset++) << 4);
          if (!registers)
            return _URC_FAILURE;
          if (registers & (1<<15))
            wrotePC = true;
          _Unwind_VRS_Pop(context, _UVRSC_CORE, registers, _UVRSD_UINT32);
          break;
        }
        case 0x90: {
          uint8_t reg = byte & 0x0f;
          if (reg == 13 || reg == 15)
            return _URC_FAILURE;
          uint32_t sp;
          _Unwind_VRS_Get(context, _UVRSC_CORE, UNW_ARM_R0 + reg,
                          _UVRSD_UINT32, &sp);
          _Unwind_VRS_Set(context, _UVRSC_CORE, UNW_ARM_SP, _UVRSD_UINT32,
                          &sp);
          break;
        }
        case 0xa0: {
          uint8_t numRegisters = 1 + (byte & 0x07);
          uint16_t registers = ((1<<numRegisters) - 1) << 4;
          if (byte & 0x08)
            registers |= 1<<14;
          _Unwind_VRS_Pop(context, _UVRSC_CORE, registers, _UVRSD_UINT32);
          break;
        }
        case 0xb0: {
          switch (byte) {
            case 0xb0:
              finish = true;
              break;
            case 0xb1: {
              if (offset >= len)
                return _URC_FAILURE;
              uint8_t registers = getByte(data, offset++);
              if (registers & 0xf0 || !registers)
                return _URC_FAILURE;
              _Unwind_VRS_Pop(context, _UVRSC_CORE, registers, _UVRSD_UINT32);
              break;
            }
            case 0xb2: {
              uint32_t addend = 0;
              while (true) {
                if (offset >= len)
                  return _URC_FAILURE;
                uint8_t v = getByte(data, offset++);
                addend = addend << 7 | (v & 0x7f);
                if ((v & 0x80) == 0)
                  break;
              }
              uint32_t sp;
              _Unwind_VRS_Get(context, _UVRSC_CORE, UNW_ARM_SP,
                              _UVRSD_UINT32, &sp);
              sp += 0x204 + addend;
              _Unwind_VRS_Set(context, _UVRSC_CORE, UNW_ARM_SP,
                              _UVRSD_UINT32, &sp);
              break;
            }
            case 0xb3:
              // TODO(piman): pop VFP single precision from FSTMFDX.
              // (Only in phase 2, see 4.7 on lazy saving of these)
              return _URC_FAILURE;
            case 0xb4:
            case 0xb5:
            case 0xb6:
            case 0xb7:
              return _URC_FAILURE;
            default:
              // TODO(piman): pop VFP double precision from FSTMFDX.
              // (Only in phase 2, see 4.7 on lazy saving of these)
              return _URC_FAILURE;
          }
          break;
        }
        default:
          // TODO(piman): iwMMX, VFP double precision from FSTMFDD, spares.
          // (Only in phase 2, see 4.7 on lazy saving of these)
          return _URC_FAILURE;
      }
    }
  }
  if (!wrotePC) {
    uint32_t lr;
    _Unwind_VRS_Get(context, _UVRSC_CORE, UNW_ARM_LR, _UVRSD_UINT32, &lr);
    _Unwind_VRS_Set(context, _UVRSC_CORE, UNW_ARM_IP, _UVRSD_UINT32, &lr);
  }
  return _URC_CONTINUE_UNWIND;
}

extern "C" _Unwind_Reason_Code __aeabi_unwind_cpp_pr0(
    _Unwind_State state,
    _Unwind_Control_Block *ucbp,
    _Unwind_Context *context) {
  return unwindOneFrame(state, ucbp, context);
}

extern "C" _Unwind_Reason_Code __aeabi_unwind_cpp_pr1(
    _Unwind_State state,
    _Unwind_Control_Block *ucbp,
    _Unwind_Context *context) {
  return unwindOneFrame(state, ucbp, context);
}

extern "C" _Unwind_Reason_Code __aeabi_unwind_cpp_pr2(
    _Unwind_State state,
    _Unwind_Control_Block *ucbp,
    _Unwind_Context *context) {
  return unwindOneFrame(state, ucbp, context);
}

#endif  // __arm__ && !CXXABI_SJLJ
