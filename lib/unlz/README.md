# unlz

Small decompression core for the X68000 LZ stream variant found in compressed
MDX/PDX files.

This code is based on the token model of Fabrice Bellard's MIT-licensed
original LZEXE source code, with stream-layout differences verified from
compressed and decoded MDX/PDX samples:

- 8-bit MSB-first control bits.
- Short copies use a 1-byte negative distance and 2-bit length code.
- Long copies use a big-endian 16-bit field sign-extended before a 3-bit
  arithmetic shift.

The library only provides the decompression core and stream marker probing.
It does not parse MDX or PDX containers and is not connected to playback yet.
