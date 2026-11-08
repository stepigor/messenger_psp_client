"""Read PSP SFO metadata, requiring the ordered tables expected by XMB."""
import struct


def read_sfo(blob):
    assert len(blob) >= 20, "SFO header length"
    magic, version, keys, data, count = struct.unpack_from('<5I', blob)
    assert magic == 0x46535000 and version == 0x101, "SFO header"
    assert count <= 256 and 20 + 16 * count <= keys <= data <= len(blob), "SFO tables"
    assert data % 4 == 0, "SFO data alignment"
    result = {}
    previous = ''
    for i in range(count):
        key, kind, size, capacity, at = struct.unpack_from('<HHIII', blob, 20 + 16 * i)
        end = blob.find(b'\0', keys + key, data)
        assert keys + key < end < data and size <= capacity and data + at + capacity <= len(blob), "SFO entry bounds"
        assert at % 4 == 0 and capacity % 4 == 0, "SFO value alignment"
        name = blob[keys + key:end].decode('ascii')
        assert name not in result, "duplicate SFO key"
        assert name > previous, "SFO key order (XMB requires sorted keys)"
        previous = name
        value = blob[data + at:data + at + size]
        if kind == 0x0404:
            assert size == capacity == 4, "SFO integer"
            result[name] = struct.unpack('<I', value)[0]
        else:
            assert kind == 0x0204 and value.endswith(b'\0') and b'\0' not in value[:-1], "SFO string"
            result[name] = value[:-1].decode('utf-8')
    return result
