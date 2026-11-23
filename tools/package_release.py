"""Package a built War in the North camera for Windows x64 and Linux with Proton."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = ("windows-x64", "linux-proton")
SHARED_FILES = (
    "LICENSE",
    "README.md",
    "requirements.txt",
    "tools/export_photo.py",
    "tools/grade.py",
    "tools/linear_dng.py",
)


def text_bytes(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def package(version, output):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?", version):
        raise ValueError("Use a version such as 0.1.0 or 0.1.0-beta.1")
    shared = {name: text_bytes(ROOT / name) for name in SHARED_FILES}
    shared["dbghelp.dll"] = (ROOT / "build/dbghelp.dll").read_bytes()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        stage = Path(temporary)
        checksums = []
        products = []
        for platform in PLATFORMS:
            name = f"witn-photo-camera-{version}-{platform}"
            archive = stage / f"{name}.zip"
            guide = text_bytes(ROOT / "release" / f"install-{platform}.txt")
            files = {**shared, "INSTALL.txt": guide}
            with zipfile.ZipFile(archive, "w") as bundle:
                for member, payload in sorted(files.items()):
                    # Fixed timestamps and file modes make the ZIP independent of the checkout.
                    entry = zipfile.ZipInfo(f"{name}/{member}", date_time=(1980, 1, 1, 0, 0, 0))
                    entry.create_system = 3
                    entry.external_attr = 0o100644 << 16
                    bundle.writestr(entry, payload, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
            checksums.append(f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")
            products.append(archive)
        manifest = stage / "SHA256SUMS"
        manifest.write_bytes("".join(checksums).encode("ascii"))
        products.append(manifest)
        for product in products:
            os.replace(product, output / product.name)
        return [output / product.name for product in products]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="Release version without the v prefix")
    parser.add_argument("--output", required=True, type=Path, help="Destination for both ZIPs and SHA256SUMS")
    args = parser.parse_args()
    try:
        for product in package(args.version, args.output):
            print(product)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
