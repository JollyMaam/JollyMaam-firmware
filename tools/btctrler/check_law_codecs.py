#!/usr/bin/env python3
"""Exhaustive BD19 codec checks against an independent fixed-width IR model.

Expected operations come from occurrence 1 au_law_codec.c.o, not from a
standard codec library: in particular, A-law wraps the negation of -32768.
This checks arithmetic and callback wiring, not audio hardware or timing.
"""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile


ENDS = (255, 511, 1023, 2047, 4095, 8191, 16383, 32767)


def signed16(value):
    return ((value + 32768) & 65535) - 32768


def encode(sample, alaw):
    if alaw:
        # IR %13=sub i16; %16=sext i16; signed threshold search.
        magnitude = sample if sample >= 0 else signed16(-sample)
        mask = 213 if sample >= 0 else 85
    else:
        # IR %15/%17 keep bias/negation in i32.
        magnitude = sample + 132 if sample >= 0 else 132 - sample
        mask = 255 if sample >= 0 else 127
    segment = next((n for n, end in enumerate(ENDS) if magnitude <= end), 8)
    if segment == 8:
        return mask ^ 127 if alaw else mask & 128
    shift = 4 if alaw and segment < 2 else segment + 3
    return (((magnitude >> shift) & 15) | (segment << 4)) ^ mask


def decode(code, alaw):
    if alaw:
        value = code ^ 85
        segment = (value >> 4) & 7
        low = (value << 4) & 240
        if segment == 0:
            magnitude = low | 8
        elif segment == 1:
            magnitude = low | 264
        else:
            magnitude = signed16((low | 264) << (segment - 1))
        return magnitude if code & 128 else signed16(-magnitude)
    # Original %20 compares the ORIGINAL i8 code as signed, before select.
    inverted = code ^ 255
    magnitude = (((inverted << 3) & 120) | 132) << ((inverted >> 4) & 7)
    return signed16(132 - magnitude if code < 128 else magnitude + 65404)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='bd19-law-codecs-') as tmp:
        library = Path(tmp) / 'codecs.so'
        sources = [root / 'src/platform/jieli/btctrler/bd19' / name for name in
                   ('au_law_codec.c', 'esco_codec.c')]
        subprocess.run(['gcc', '-O2', '-std=gnu11', '-shared', '-fPIC',
                        '-DUNITTEST', '-DCONFIG_CPU_BD19',
                        '-DBTCTRLER_CODEC_LIBRARY_TEST', '-iquote', str(root / 'src'),
                        *map(str, sources), '-o', str(library)], check=True)
        dll = C.CDLL(str(library))
        pcm = C.POINTER(C.c_int16)
        byte = C.POINTER(C.c_uint8)

        class Codec(C.Structure):
            _fields_ = [
                ('need_buf', C.CFUNCTYPE(C.c_uint32)),
                ('init', C.CFUNCTYPE(C.c_uint32, C.c_void_p)),
                ('encode', C.CFUNCTYPE(C.c_uint32, C.c_void_p, pcm, byte, C.c_int16)),
                ('decode', C.CFUNCTYPE(C.c_uint32, C.c_void_p, byte, pcm, C.c_int16)),
            ]

        tables = [Codec.in_dll(dll, 'esco_ulaw_codec'), Codec.in_dll(dll, 'esco_alaw_codec')]
        selector = dll.esco_codec_for_type
        selector.argtypes = [C.c_int]
        selector.restype = C.POINTER(Codec)
        for kind, table in enumerate(tables):
            require(C.addressof(selector(kind).contents) == C.addressof(table), 'selector wiring')
            require(table.need_buf() == 1, 'state allocation size')
            state = (C.c_uint8 * 3)(0x91, 0x82, 0x73)
            require(table.init(C.byref(state, 1)) == 0 and list(state) == [0x91, 0, 0x73],
                    'init must clear exactly one byte')
            for length in (0, -1, -32768):
                require(table.encode(None, None, None, length) == 0, 'nonpositive encode')
                require(table.decode(None, None, None, length) == 0, 'nonpositive decode')
            codes = (C.c_uint8 * 256)(*range(256))
            decoded = (C.c_int16 * 258)(*([1234] * 258))
            require(table.decode(None, codes, C.cast(C.byref(decoded, 2), pcm), 256) == 0,
                    'decode return')
            require(decoded[0] == decoded[257] == 1234, 'decode sentinels')
            expected = [decode(code, kind == 1) for code in range(256)]
            require(list(decoded)[1:-1] == expected, f'codec {kind}: all decoded bytes')
            # Chunking also exercises the maximum positive signed16 length.
            for start in range(-32768, 32768, 32767):
                values = list(range(start, min(start + 32767, 32768)))
                samples = (C.c_int16 * len(values))(*values)
                encoded = (C.c_uint8 * (len(values) + 2))(*([0x6b] * (len(values) + 2)))
                require(table.encode(None, samples, C.cast(C.byref(encoded, 1), byte),
                                     len(values)) == 0, 'encode return')
                require(encoded[0] == encoded[len(values) + 1] == 0x6b, 'encode sentinels')
                expected = [encode(sample, kind == 1) for sample in values]
                require(list(encoded)[1:-1] == expected, f'codec {kind}: samples from {start}')
        for kind in (-2147483648, -1, 2, 3, 4, 2147483647):
            require(not selector(kind), f'unsupported codec {kind}')
    print('BD19 law codecs: 131072 encode and 512 decode values passed; ABI/selector checks passed')


if __name__ == '__main__':
    main()
