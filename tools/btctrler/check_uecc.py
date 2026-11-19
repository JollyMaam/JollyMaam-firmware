#!/usr/bin/env python3
"""Exercise BD19 curve callbacks with an independent Python integer backend.

Run from the repository root: python3 tools/btctrler/check_uecc.py
This checks recovered specialization arithmetic, not the device ROM backend.
"""
import ctypes as C
from pathlib import Path
import random
import subprocess
import tempfile

SHIM = r'''
#define UNITTEST
#define CONFIG_CPU_BD19
#include "btctrler/glue.h"
#include "btctrler/bd19/uECC_curves.inc"
u32 (*review_op)(int, u32 *, const u32 *, const u32 *, const u32 *, int, int);
u32 uECC_vli_isZero(const u32 *x, wordcount_t n) { return review_op(0,0,x,0,0,n,0); }
u32 uECC_vli_testBit(const u32 *x, bitcount_t b) { return review_op(1,0,x,0,0,b/32+1,b); }
void uECC_vli_set(u32 *d,const u32 *x,wordcount_t n) { review_op(2,d,x,0,0,n,0); }
void uECC_vli_rshift1(u32 *d,wordcount_t n) { review_op(3,d,d,0,0,n,0); }
u32 uECC_vli_add(u32 *d,const u32 *x,const u32 *y,wordcount_t n) { return review_op(4,d,x,y,0,n,0); }
u32 uECC_vli_sub(u32 *d,const u32 *x,const u32 *y,wordcount_t n) { return review_op(5,d,x,y,0,n,0); }
s8 uECC_vli_cmp_unsafe(const u32 *x,const u32 *y,wordcount_t n) { return review_op(6,0,x,y,0,n,0); }
void uECC_vli_clear(u32 *d,wordcount_t n) { review_op(7,d,0,0,0,n,0); }
void uECC_vli_modSquare_fast(u32 *d,const u32 *x,uECC_Curve c) { review_op(8,d,x,x,c->p,c->num_words,0); }
void uECC_vli_modMult_fast(u32 *d,const u32 *x,const u32 *y,uECC_Curve c) { review_op(8,d,x,y,c->p,c->num_words,0); }
void uECC_vli_modAdd(u32 *d,const u32 *x,const u32 *y,const u32 *p,wordcount_t n) { review_op(9,d,x,y,p,n,0); }
void uECC_vli_modSub(u32 *d,const u32 *x,const u32 *y,const u32 *p,wordcount_t n) { review_op(10,d,x,y,p,n,0); }
void review_reduce(uECC_Curve c,u32 *d,u32 *p) { c->mmod_fast(d,p); }
void review_side(uECC_Curve c,u32 *d,u32 *x) { c->x_side(d,x,c); }
void review_double(uECC_Curve c,u32 *x,u32 *y,u32 *z) { c->double_jacobian(x,y,z,c); }
const u32 *review_param(uECC_Curve c,int i) { return i==0?c->p:i==1?c->G:c->b; }
'''


def main():
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory() as tmp:
        src, lib = Path(tmp) / 'shim.c', Path(tmp) / 'shim.so'
        src.write_text(SHIM)
        subprocess.run(['gcc', '-O2', '-shared', '-fPIC', '-iquote', str(root / 'src'),
                        '-iquote', str(root / 'src'), str(src), '-o', str(lib)], check=True)
        dll = C.CDLL(str(lib))
        word = C.c_uint32
        ptr = C.POINTER(word)
        operation = C.CFUNCTYPE(word, C.c_int, ptr, ptr, ptr, ptr, C.c_int, C.c_int)

        def integer(p, n):
            return sum(int(p[i]) << (32 * i) for i in range(n)) if p else 0

        def put(p, value, n):
            for i in range(n):
                p[i] = (value >> (32 * i)) & 0xffffffff

        def backend(op, dst, left, right, modulus, n, bit):
            x, y, p = integer(left, n), integer(right, n), integer(modulus, n)
            if op == 0: return int(x == 0)
            if op == 1: return (x >> bit) & 1
            if op == 6: return ((x > y) - (x < y)) & 0xffffffff
            carry = 0
            if op == 2: value = x
            elif op == 3: value = x >> 1
            elif op == 4: value = x + y; carry = value >> (32 * n)
            elif op == 5: value = x - y; carry = int(value < 0)
            elif op == 7: value = 0
            elif op == 8: value = x * y % p
            elif op == 9: value = (x + y) % p
            elif op == 10: value = (x - y) % p
            else: raise AssertionError(op)
            put(dst, value, n)
            return carry

        callback = operation(backend)
        C.c_void_p.in_dll(dll, 'review_op').value = C.cast(callback, C.c_void_p).value
        dll.review_param.argtypes = [C.c_void_p, C.c_int]
        dll.review_param.restype = ptr
        dll.review_reduce.argtypes = [C.c_void_p, ptr, ptr]
        dll.review_side.argtypes = [C.c_void_p, ptr, ptr]
        dll.review_double.argtypes = [C.c_void_p, ptr, ptr, ptr]
        rng = random.Random(0xbd19)
        count = 0
        for name, n, a in [('160r1',5,-3), ('192r1',6,-3), ('224r1',7,-3),
                           ('256r1',8,-3), ('256k1',8,0)]:
            selector = getattr(dll, 'uECC_secp' + name)
            selector.restype = C.c_void_p
            curve = selector()
            p = integer(dll.review_param(curve, 0), n)
            b = integer(dll.review_param(curve, 2), n)
            def array(value, words=n):
                result = (word * words)()
                put(result, value, words)
                return result
            samples = [0, 1, p-1, p, p+1, p*p-1, (1 << (64*n))-1]
            samples += [rng.getrandbits(64*n) for _ in range(100)]
            for value in samples:
                out, product = array(0), array(value, 2*n)
                dll.review_reduce(curve, out, product)
                got = integer(out, n)
                # The SDK's 160r1/k1 strict > comparison retains p instead of 0.
                expected = value % p
                assert got == expected or (name in ('160r1','256k1') and got == p and expected == 0), (name,value,got,expected)
                count += 1
            for _ in range(20):
                x, y, z = rng.randrange(p), rng.randrange(1,p), rng.randrange(1,p)
                out = array(0)
                dll.review_side(curve, out, array(x))
                assert integer(out,n) == (x*x*x + a*x + b) % p
                # Scale affine coordinates to Jacobian, then compare affine doubling.
                X,Y,Z = array(x*z*z % p),array(y*z*z*z % p),array(z)
                dll.review_double(curve,X,Y,Z)
                zi = pow(integer(Z,n), -1, p)
                xx, yy = integer(X,n)*zi**2 % p, integer(Y,n)*zi**3 % p
                slope = (3*x*x + a)*pow(2*y,-1,p) % p
                expected_x = (slope*slope-2*x) % p
                assert xx == expected_x and yy == (slope*(x-expected_x)-y) % p, name
                count += 2
            X,Y,Z = array(7),array(11),array(0)
            dll.review_double(curve,X,Y,Z)
            assert integer(X,n)==7 and integer(Y,n)==11 and integer(Z,n)==0
            count += 1
        print(f'BD19 curve arithmetic: {count} checks passed (Python integer backend)')


if __name__ == '__main__':
    main()
