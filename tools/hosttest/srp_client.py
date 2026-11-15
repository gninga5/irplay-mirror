#!/usr/bin/env python3
"""Lato client (iPhone) di SRP-6a nella variante Apple, scritto in modo indipendente dal C++.

Uso: srp_client.py <user> <pin> <salt_hex> <B_hex> [a_hex]
Stampa: A_hex M1_hex M2_atteso_hex K_hex

Serve ai test: se il server C++ e questo client calcolano le stesse prove, l'implementazione
segue la specifica (RFC 5054 con le modifiche di Apple), non soltanto se stessa.
"""
import hashlib
import os
import sys

N = int(
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73", 16)
g = 2
NLEN = 256


def b(n):           # big-endian senza zeri iniziali
    return n.to_bytes((n.bit_length() + 7) // 8, "big")


def pad(n):         # big-endian portato a 256 byte
    return n.to_bytes(NLEN, "big")


def H(*parts):
    h = hashlib.sha1()
    for p in parts:
        h.update(p)
    return h.digest()


def main():
    user, pin, salt_hex, B_hex = sys.argv[1:5]
    salt = bytes.fromhex(salt_hex)
    B = int(B_hex, 16)
    a = int(sys.argv[5], 16) if len(sys.argv) > 5 else int.from_bytes(os.urandom(32), "big")

    k = int.from_bytes(H(pad(N), pad(g)), "big")
    x = int.from_bytes(H(salt, H(user.encode(), b":", pin.encode())), "big")
    A = pow(g, a, N)
    u = int.from_bytes(H(pad(A), pad(B)), "big")
    S = pow((B - k * pow(g, x, N)) % N, a + u * x, N)
    K = H(b(S), b"\x00\x00\x00\x00") + H(b(S), b"\x00\x00\x00\x01")
    hN, hg = H(b(N)), H(b(g))
    M1 = H(bytes(i ^ j for i, j in zip(hN, hg)), H(user.encode()), salt, b(A), b(B), K)
    M2 = H(b(A), M1, K)
    print(b(A).hex(), M1.hex(), M2.hex(), K.hex())


if __name__ == "__main__":
    main()
