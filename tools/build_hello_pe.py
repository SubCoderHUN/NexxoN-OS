#!/usr/bin/env python3
"""Build hello.exe: minimal amd64 PE with KERNEL32.ExitProcess import."""
import struct

OUT = "tools/hello_pe.bin"
IMAGE_BASE = 0x40000000
TEXT_RVA = 0x1000
IDATA_RVA = 0x2000
ENTRY_RVA = TEXT_RVA
SIZE_OF_IMAGE = 0x3000

# Offsets within the .idata section (non-overlapping).
IDATA_DESC   = 0x00
IDATA_ILT    = 0x20
IDATA_IAT    = 0x30
IDATA_NAME   = 0x80
IDATA_HINT   = 0xA0

ILT_RVA = IDATA_RVA + IDATA_ILT
IAT_RVA = IDATA_RVA + IDATA_IAT
NAME_RVA = IDATA_RVA + IDATA_NAME
HINT_NAME_RVA = IDATA_RVA + IDATA_HINT

# amd64: sub rsp,0x28; xor ecx,ecx; call [ExitProcess IAT]; add rsp,0x28; ret
text = bytearray()
text += b"\x48\x83\xEC\x28"              # sub rsp, 0x28
text += b"\x48\x31\xC9"                  # xor rcx, rcx
call_rva = TEXT_RVA + len(text)
text += b"\xFF\x15"                      # call qword ptr [rip+disp32]
disp = IAT_RVA - (call_rva + 6)
text += struct.pack("<i", disp)
text += b"\x48\x83\xC4\x28"              # add rsp, 0x28
text += b"\xC3"                          # ret
while len(text) < 0x200:
    text.append(0)

idata = bytearray(0x200)
# IMAGE_IMPORT_DESCRIPTOR (correct field order).
struct.pack_into("<I", idata, IDATA_DESC + 0, ILT_RVA)        # OriginalFirstThunk
struct.pack_into("<I", idata, IDATA_DESC + 4, 0)             # TimeDateStamp
struct.pack_into("<I", idata, IDATA_DESC + 8, 0)             # ForwarderChain
struct.pack_into("<I", idata, IDATA_DESC + 12, NAME_RVA)     # Name
struct.pack_into("<I", idata, IDATA_DESC + 16, IAT_RVA)      # FirstThunk
# ILT + IAT entries (hint/name RVA); loader patches IAT to our stub VA.
struct.pack_into("<Q", idata, IDATA_ILT, HINT_NAME_RVA)
struct.pack_into("<Q", idata, IDATA_IAT, HINT_NAME_RVA)
# DLL name + import name.
dll = b"KERNEL32.dll\x00"
idata[IDATA_NAME:IDATA_NAME + len(dll)] = dll
struct.pack_into("<H", idata, IDATA_HINT, 0)
fn = b"ExitProcess\x00"
idata[IDATA_HINT + 2:IDATA_HINT + 2 + len(fn)] = fn

dos = bytearray(128)
dos[0:2] = b"MZ"
struct.pack_into("<I", dos, 0x3C, 0x80)
img = bytearray(dos)
img += b"PE\0\0"
img += struct.pack("<H", 0x8664)   # Machine
img += struct.pack("<H", 2)        # sections
img += struct.pack("<I", 0)
img += struct.pack("<I", 0)
img += struct.pack("<I", 0)
img += struct.pack("<H", 0xF0)
img += struct.pack("<H", 0x22)
opt = bytearray(0xF0)
struct.pack_into("<H", opt, 0, 0x20B)
struct.pack_into("<I", opt, 16, ENTRY_RVA)
struct.pack_into("<I", opt, 20, TEXT_RVA)
struct.pack_into("<Q", opt, 24, IMAGE_BASE)
struct.pack_into("<I", opt, 32, 0x1000)
struct.pack_into("<I", opt, 36, 0x200)
struct.pack_into("<I", opt, 56, SIZE_OF_IMAGE)
struct.pack_into("<I", opt, 60, 0x400)
struct.pack_into("<H", opt, 68, 3)
struct.pack_into("<I", opt, 108, 16)          # NumberOfRvaAndSizes
struct.pack_into("<I", opt, 120, IDATA_RVA)  # Import table RVA (dir entry 1)
struct.pack_into("<I", opt, 124, 0x50)      # Import table size
img += opt

def section(name, vaddr, raw_size, raw_ptr, chars):
    sec = bytearray(40)
    sec[0:8] = name.encode("ascii")[:8].ljust(8, b"\x00")
    struct.pack_into("<I", sec, 8, max(raw_size, 0x200))
    struct.pack_into("<I", sec, 12, vaddr)
    struct.pack_into("<I", sec, 16, raw_size)
    struct.pack_into("<I", sec, 20, raw_ptr)
    struct.pack_into("<I", sec, 36, chars)
    return sec

headers_end = 0x400
img += section(".text", TEXT_RVA, len(text), headers_end, 0x60000020)
img += section(".idata", IDATA_RVA, len(idata), headers_end + len(text), 0xC0000040)
while len(img) < headers_end:
    img.append(0)
img += text
img += idata
while len(img) % 0x200:
    img.append(0)

open(OUT, "wb").write(img)
print(f"wrote {OUT} ({len(img)} bytes)")
