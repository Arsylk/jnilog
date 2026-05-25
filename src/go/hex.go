package main

// Tiny alloc-free hex helpers used on the per-arg formatter hot path. Lives
// without a build tag so both the android-tagged logger.go and the !android
// types_host.go mirror reference the same implementation (drift-proof).
//
// `fmt.Sprintf("0x%02x", ...)` was allocating an extra []byte + format-state
// per call; these versions write straight into a tiny stack buffer and
// `string()`-convert once.

const hexDigits = "0123456789abcdef"

func byteHex(b uint8) string {
	buf := [4]byte{'0', 'x', hexDigits[b>>4], hexDigits[b&0xf]}
	return string(buf[:])
}

func u16Hex(v uint16) string {
	buf := [4]byte{
		hexDigits[(v>>12)&0xf], hexDigits[(v>>8)&0xf],
		hexDigits[(v>>4)&0xf], hexDigits[v&0xf],
	}
	return string(buf[:])
}

func u32Hex(v uint32) string {
	buf := [8]byte{
		hexDigits[(v>>28)&0xf], hexDigits[(v>>24)&0xf],
		hexDigits[(v>>20)&0xf], hexDigits[(v>>16)&0xf],
		hexDigits[(v>>12)&0xf], hexDigits[(v>>8)&0xf],
		hexDigits[(v>>4)&0xf], hexDigits[v&0xf],
	}
	return string(buf[:])
}
