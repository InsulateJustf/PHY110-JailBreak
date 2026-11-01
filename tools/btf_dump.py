#!/usr/bin/env python3
"""Minimal BTF reader: dump struct member offsets and sizes from a .BTF blob."""
import struct
import sys

KIND = {0: "VOID", 1: "INT", 2: "PTR", 3: "ARRAY", 4: "STRUCT", 5: "UNION",
        6: "ENUM", 7: "FWD", 8: "TYPEDEF", 9: "VOLATILE", 10: "CONST",
        11: "RESTRICT", 12: "FUNC", 13: "FUNC_PROTO", 14: "VAR", 15: "DATASEC",
        16: "FLOAT", 17: "DECL_TAG", 18: "TYPE_TAG", 19: "ENUM64"}


class BTF:
    def __init__(self, blob):
        magic, ver, flags, hdr_len, type_off, type_len, str_off, str_len = \
            struct.unpack_from("<HBBIIIII", blob, 0)
        if magic != 0xEB9F:
            raise SystemExit(f"bad BTF magic {magic:#x}")
        self.strs = blob[hdr_len + str_off: hdr_len + str_off + str_len]
        self.types = []
        off = hdr_len + type_off
        end = off + type_len
        while off < end:
            name_off, info, size_type = struct.unpack_from("<III", blob, off)
            off += 12
            kind = (info >> 24) & 0x1F
            vlen = info & 0xFFFF
            kflag = (info >> 31) & 1
            extra = b""
            if kind in (1, 14):
                extra = blob[off:off + 4]
                off += 4
            elif kind == 3:
                extra = blob[off:off + 12]
                off += 12
            elif kind in (4, 5):
                extra = blob[off:off + vlen * 12]
                off += vlen * 12
            elif kind == 6:
                extra = blob[off:off + vlen * 8]
                off += vlen * 8
            elif kind == 13:
                extra = blob[off:off + vlen * 8]
                off += vlen * 8
            elif kind == 15:
                extra = blob[off:off + vlen * 12]
                off += vlen * 12
            elif kind == 17:
                extra = blob[off:off + 4]
                off += 4
            elif kind == 19:
                extra = blob[off:off + vlen * 12]
                off += vlen * 12
            self.types.append((self.name(name_off), kind, vlen, kflag, size_type, extra))

    def name(self, off):
        if not off:
            return ""
        end = self.strs.find(b"\0", off)
        return self.strs[off:end].decode("utf-8", "replace")

    def find(self, name, kind=None):
        for idx, t in enumerate(self.types):
            if t[0] == name and (kind is None or t[1] == kind):
                return idx, t
        raise SystemExit(f"type {name} not found")

    def members(self, idx):
        name, kind, vlen, kflag, size_type, extra = self.types[idx]
        out = []
        for i in range(vlen):
            moff, mtype, mbits = struct.unpack_from("<III", extra, i * 12)
            # offset encoding depends on kflag==0 (bit offset) vs 1 (bitfield)
            out.append((self.name(moff), mtype, mbits))
        return out

    def describe(self, idx, depth=0, seen=None):
        name, kind, vlen, kflag, size_type, extra = self.types[idx]
        print(f"{'  ' * depth}{name or '<anon>'}: {KIND.get(kind, kind)} size={size_type} vlen={vlen}")
        if kind in (4, 5):
            for mname, mtype, mbits in self.members(idx):
                tname = self.types[mtype][0] if mtype < len(self.types) else "?"
                if kflag:
                    bit_off = mbits & 0xFFFFFF
                    bitsz = mbits >> 24
                    print(f"{'  ' * depth}  {mname:32s} off={bit_off // 8:#06x} "
                          f"bits={bitsz} type={tname}")
                else:
                    print(f"{'  ' * depth}  {mname:32s} off={mbits // 8:#06x} type={tname}")

    def offset_of(self, typename, member):
        idx, _ = self.find(typename)
        for mname, mtype, mbits in self.members(idx):
            if mname == member:
                return (mbits & 0xFFFFFF if self.types[idx][3] else mbits) // 8
        raise SystemExit(f"{typename}.{member} not found")


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        blob = f.read()
    btf = BTF(blob)
    print(f"types={len(btf.types)}")
    for t in ("task_struct", "cred", "k_itimer"):
        try:
            idx, _ = btf.find(t)
        except SystemExit as e:
            print(e)
            continue
        btf.describe(idx)
        print()

    print("=== key offsets ===")
    for tname, member in [("task_struct", "real_cred"), ("task_struct", "cred"),
                          ("task_struct", "comm"), ("creds", "x")]:
        try:
            print(f"{tname}.{member} = {btf.offset_of(tname, member):#x}")
        except SystemExit as e:
            print(e)
    idx, t = btf.find("k_itimer")
    print(f"sizeof(k_itimer) = {t[4]:#x}")


if __name__ == "__main__":
    main()
