# pspdecrypt (subset)

The PRX decrypter and the KIRK engine of [pspdecrypt](https://github.com/John-K/pspdecrypt)
(commit c156627, 2024-06-07), unchanged, under its licence: **GPL-3.0** (`LICENSE.TXT`).
Used by the Android app to decrypt the `EBOOT.BIN` of the player's own ISO
(`runtime/common/iso_eboot.c`, docs/35). `pspdecrypt_shim.cpp` is ours: a C entry point.

Copied files: `PrxDecrypter.cpp/.h`, `CommonTypes.h`, `Swap.h`, `libkirk/{kirk_engine,AES,SHA1}.c/.h`,
`libkirk/bn.c`, `libkirk/ec.c`. The desktop tools keep using the full program in `vendor/pspdecrypt`.

An APK that carries this code is GPL-3: whoever receives it is entitled to the source.
