#!/usr/bin/env python3
# Extract members of a PCSX2 .p2s (zip with zstd, method 93) using PCSX2's own zstd.dll.
import ctypes, struct, sys, zipfile
src, outdir, names = sys.argv[1], sys.argv[2], sys.argv[3:]
zstd = ctypes.CDLL(r'D:/PCSX2/zstd.dll')
zstd.ZSTD_decompress.restype = ctypes.c_size_t
zstd.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
z = zipfile.ZipFile(src)
raw = open(src, 'rb').read()
for info in z.infolist():
    if names and info.filename not in names:
        continue
    off = info.header_offset
    nlen, xlen = struct.unpack_from('<HH', raw, off + 26)
    data = raw[off + 30 + nlen + xlen: off + 30 + nlen + xlen + info.compress_size]
    if info.compress_type == 0:
        out = data
    else:
        buf = ctypes.create_string_buffer(info.file_size)
        n = zstd.ZSTD_decompress(buf, info.file_size, data, len(data))
        if zstd.ZSTD_isError(n):
            print('error', info.filename); continue
        out = buf.raw[:n]
    open(f'{outdir}/{info.filename}', 'wb').write(out)
    print(info.filename, len(out), 'method', info.compress_type)
