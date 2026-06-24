#!/usr/bin/env python3
import sys
import struct
import mmap


def write_config(freq, var, version):
    with open("/dev/shm/myshm", "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        
        # Write freq and var first
        mm.seek(8)  # Skip is_used (8 bytes)
        mm.write(struct.pack('<QQ', freq, var))
        mm.flush()
        
        # Then set is_used flag atomically (write at offset 0)
        mm.seek(0)
        mm.write(struct.pack('<Q', version))
        mm.flush()
        
        print(f"Config written: freq={freq}, var={var}")
        mm.close()

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: ./write_config.py <freq> <var> <version>")
        sys.exit(1)
    
    write_config(int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]))