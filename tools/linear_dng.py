#!/usr/bin/env python3
"""Export a scene-linear Rec.709/D65 EXR as a floating-point RGB LinearRaw DNG.

This product includes DNG technology under license by Adobe.
"""

import argparse
import math
import tempfile
from pathlib import Path

import numpy as np
from PIL import ExifTags, TiffTags
from PIL.TiffImagePlugin import IFDRational, ImageFileDirectory_v2

from grade import REC709, publish_new_file, read_photo_exr, rgb_to_xyz

# DNG ColorMatrix1 maps XYZ under CalibrationIlluminant1 (D65) to camera RGB.
# ForwardMatrix1 maps white-balanced camera RGB to the D50 profile connection
# space. Derive both from the same Rec.709 primaries and Bradford adaptation.
REC709_XYZ_D65 = rgb_to_xyz(REC709)
BRADFORD = np.array([
    [0.8951, 0.2664, -0.1614],
    [-0.7502, 1.7135, 0.0367],
    [0.0389, -0.0685, 1.0296],
])
D50_XYZ = np.array([0.9642, 1.0, 0.8249])
CAMERA = "War in the North Virtual Camera"
WHITE = 65535
STRIP_ROWS = 32


def signed_rationals(matrix):
    return tuple(IFDRational(round(float(v) * 1_000_000), 1_000_000) for v in matrix.flat)


def raw_header(width, height, stops, integer, exif=None):
    bits = 16 if integer else 32
    row_bytes = width * 3 * (bits // 8)
    if row_bytes * height >= 2**32:
        raise ValueError("Image exceeds the 4 GiB classic DNG limit")
    ifd = ImageFileDirectory_v2()
    strip_starts = range(0, height, STRIP_ROWS)
    d65 = REC709_XYZ_D65.sum(axis=1)
    white_adaptation = np.diag((BRADFORD @ D50_XYZ) / (BRADFORD @ d65))
    forward = np.linalg.solve(BRADFORD, white_adaptation @ BRADFORD @ REC709_XYZ_D65)

    # Pillow writes classic TIFF in little-endian order and relocates the strip
    # offsets relative to the end of this IFD's out-of-line metadata.
    tags = {
        254: (TiffTags.LONG, 0),                  # Main image
        256: (TiffTags.LONG, width),
        257: (TiffTags.LONG, height),
        258: (TiffTags.SHORT, (bits,) * 3),
        259: (TiffTags.SHORT, 1),                 # Uncompressed
        262: (TiffTags.SHORT, 34892),             # LinearRaw
        271: (TiffTags.ASCII, "War in the North"),
        272: (TiffTags.ASCII, "Virtual Camera"),
        273: (TiffTags.LONG, tuple(y * row_bytes for y in strip_starts)),
        274: (TiffTags.SHORT, 1),
        277: (TiffTags.SHORT, 3),
        278: (TiffTags.LONG, STRIP_ROWS),
        279: (TiffTags.LONG, tuple(min(STRIP_ROWS, height - y) * row_bytes for y in strip_starts)),
        282: (TiffTags.RATIONAL, IFDRational(1)),
        283: (TiffTags.RATIONAL, IFDRational(1)),
        284: (TiffTags.SHORT, 1),                 # Interleaved RGB
        296: (TiffTags.SHORT, 1),                 # No physical resolution unit
        305: (TiffTags.ASCII, "War in the North Photo Camera"),
        339: (TiffTags.SHORT, (1 if integer else 3,) * 3),  # SampleFormat
        50706: (TiffTags.BYTE, bytes((1, 2 if integer else 4, 0, 0))),
        50707: (TiffTags.BYTE, bytes((1, 1 if integer else 4, 0, 0))),
        50708: (TiffTags.ASCII, CAMERA),
        50714: (TiffTags.SHORT, (0, 0, 0)),      # Per-channel black level
        50717: (TiffTags.LONG, (WHITE if integer else 1,) * 3),
        50720: (TiffTags.LONG, (width, height)),  # DefaultCropSize
        50721: (TiffTags.SIGNED_RATIONAL, signed_rationals(np.linalg.inv(REC709_XYZ_D65))),
        50728: (TiffTags.RATIONAL, (IFDRational(1),) * 3),  # AsShotNeutral
        50730: (TiffTags.SIGNED_RATIONAL, IFDRational(stops)),  # BaselineExposure
        50778: (TiffTags.SHORT, 21),              # CalibrationIlluminant1 = D65
        50936: (TiffTags.ASCII, "Virtual Linear Rec.709"),
        50964: (TiffTags.SIGNED_RATIONAL, signed_rationals(forward)),
    }
    if exif:
        tags[ExifTags.IFD.Exif] = (TiffTags.LONG, exif)
    for tag, (dtype, value) in tags.items():
        ifd.tagtype[tag] = dtype
        ifd[tag] = value
    header = b"II\x2a\0\x08\0\0\0" + ifd.tobytes(8)
    if len(header) + row_bytes * height >= 2**32:
        raise ValueError("Image exceeds the 4 GiB classic DNG limit")
    return header


def write_dng(rgb, destination, *, clip_negative=False, integer=False, exif=None):
    """Write a new DNG, returning (stops above scene white, clipped channel count)."""
    destination = Path(destination)
    if destination.suffix.lower() != ".dng":
        raise ValueError("Output must have the .dng extension")
    if destination.exists():
        raise FileExistsError(f"Output already exists: {destination}")
    if rgb.ndim != 3 or rgb.shape[2] != 3 or min(rgb.shape[:2]) < 1:
        raise ValueError("Expected a nonempty RGB image")
    if not np.isfinite(rgb).all():
        raise ValueError("Scene RGB contains NaN or infinity")
    negative = int(np.count_nonzero(rgb < 0))
    if integer and negative and not clip_negative:
        raise ValueError(f"{negative} negative scene channels; use --clip-negative to floor them at black")
    peak = float(rgb.max())
    # Use a power of two so BaselineExposure describes the encoding gain exactly.
    # At least one stop is available above reference white, even in a dim image.
    stops = max(1, math.ceil(math.log2(max(peak, 1.0))))
    height, width = rgb.shape[:2]
    header = raw_header(width, height, stops, integer, exif=exif)

    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".witn-dng-") as work:
        temporary = Path(work) / "photo.dng"
        with temporary.open("wb") as stream:
            stream.write(header)
            for y in range(0, height, STRIP_ROWS):
                strip = np.ldexp(rgb[y:y + STRIP_ROWS], -stops, dtype=np.float64)
                if clip_negative:
                    np.maximum(strip, 0.0, out=strip)
                if integer:
                    codes = np.rint(strip * WHITE).astype("<u2")
                else:
                    # A power-of-two exposure scale preserves the significand.
                    # Float DNG 1.4 keeps the shadow detail of an HDR game frame.
                    codes = strip.astype("<f4")
                stream.write(codes.tobytes())
        # Exclusive publication preserves existing photos, including when two
        # exports finish concurrently. The temporary lives on the same volume.
        publish_new_file(temporary, destination)
    return stops, negative if clip_negative else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--integer", action="store_true", help="16-bit unsigned DNG for older readers")
    parser.add_argument("--assume-rec709", action="store_true", help="interpret untagged scene-linear EXR as Rec.709/D65")
    parser.add_argument("--clip-negative", action="store_true", help="floor negative scene channels at zero")
    args = parser.parse_args()
    try:
        rgb, exif = read_photo_exr(args.input, assume_rec709=args.assume_rec709)
        stops, clipped = write_dng(rgb, args.output, clip_negative=args.clip_negative, integer=args.integer, exif=exif)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    storage = "16-bit integer" if args.integer else "32-bit float"
    print(f"{args.output} | {storage} linear RGB | +{stops} EV baseline | {clipped} negative channels clipped")


if __name__ == "__main__":
    main()
