"""Check the TitleID of the actual packaged CIA, not only the Makefile."""
import pathlib
import struct
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
header, _, _, certificates, ticket, tmd = struct.unpack_from("<IHHIII", data)
align = lambda n: (n + 63) & ~63
offset = align(header) + align(certificates) + align(ticket)
signature_type = struct.unpack_from(">I", data, offset)[0]
signature_size = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80,
                  0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}[signature_type]
title_id = struct.unpack_from(">Q", data, offset + signature_size + 0x4C)[0]
assert tmd >= signature_size + 0x54, "Truncated TMD"
assert title_id == 0x000400000ED07100, f"Wrong TitleID: {title_id:016X}"
assert title_id != 0x000400000BCFFF00, "This would replace the original Checkpoint"
print(f"Checkpoint GH CIA verified: TitleID {title_id:016X}")
