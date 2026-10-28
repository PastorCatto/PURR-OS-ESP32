# cat2c.py <file.cat> <c_identifier>  -> prints a const byte array in the style commands.c uses
import sys
data = open(sys.argv[1], "rb").read()
print(f"static const uint8_t {sys.argv[2]}[] = {{")
for i in range(0, len(data), 12):
    print("    " + ", ".join(f"0x{b:02x}" for b in data[i:i+12]) + ",")
print("};")
