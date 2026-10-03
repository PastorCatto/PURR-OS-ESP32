# mkmanifest.py - writes a recovery manifest for files in the current folder.
#   python mkmanifest.py 1.0.0 esp32s3 tdeck_plus kernel=kernel-tdeck_plus-1.0.0.cat:boot [more...]
# Each argument is component=file:keyrole. Prints the manifest to stdout.
import datetime, hashlib, os, sys

release, chip, board, *items = sys.argv[1:]
print(f"release={release}")
print(f"released={datetime.date.today().isoformat()}")
for item in items:
    comp, rest = item.split("=", 1)
    fname, role = rest.rsplit(":", 1)
    data = open(fname, "rb").read()
    print()
    print(f"component={comp}")
    print(f"version={release}")
    print(f"chip={chip}")
    print(f"board={board}")
    print(f"file={os.path.basename(fname)}")
    print(f"size={len(data)}")
    print(f"sha256={hashlib.sha256(data).hexdigest()}")
    print(f"key={role}")
