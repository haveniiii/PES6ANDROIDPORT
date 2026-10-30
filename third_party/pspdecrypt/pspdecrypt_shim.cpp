// PES6PORT — a C entry point to pspdecrypt's PRX decrypter (runtime/common/iso_eboot.c).
#include <cstdint>

#include "PrxDecrypter.h"

extern "C" int pes6_decrypt_prx(const uint8_t *in, uint8_t *out, uint32_t size) {
    return pspDecryptPRX(in, out, size, nullptr, false);
}
