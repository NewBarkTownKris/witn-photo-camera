"""Write full-range 16-bit log TIFFs from a scene-linear Rec.709/D65 EXR."""

import argparse
from fractions import Fraction
import math
import os
from pathlib import Path
import struct
import tempfile

import numpy as np
import OpenEXR
from PIL import ExifTags, TiffTags
from PIL.TiffImagePlugin import IFDRational


D65 = (0.3127, 0.3290)
REC709 = (0.640, 0.330, 0.300, 0.600, 0.150, 0.060, *D65)
AWG3 = (0.6840, 0.3130, 0.2210, 0.8480, 0.0861, -0.1020, *D65)
F_GAMUT = (0.708, 0.292, 0.170, 0.797, 0.131, 0.046, *D65)
PROFILES = {"logc3": "ARRI Wide Gamut 3 / LogC3 EI 800", "flog": "Fujifilm F-Gamut / F-Log"}


def capture_exif(header):
    """Translate optional geometric exposure settings, independently of pixel gain."""
    tags = {}
    for source, tag in (("expTime", ExifTags.Base.ExposureTime), ("aperture", ExifTags.Base.FNumber)):
        value = header.get(source)
        if isinstance(value, (int, float)) and math.isfinite(value) and 0 < value < 2**32:
            fraction = Fraction(value).limit_denominator(100_000)
            if 0 < fraction.numerator < 2**32:
                tags[tag] = IFDRational(fraction.numerator, fraction.denominator)
    iso = header.get("isoSpeed")
    if isinstance(iso, (int, float)) and 1 <= iso < 65535 and iso == int(iso):
        tags[ExifTags.Base.ISOSpeedRatings] = int(iso)
    programs = {"Manual": 1, "Aperture Priority": 3, "Shutter Priority": 4}
    mode = header.get("witnExposureMode")
    if isinstance(mode, str) and mode in programs:
        tags[ExifTags.Base.ExposureProgram] = programs[mode]
    return tags


def read_photo_exr(path: Path, *, assume_rec709=False):
    """Read RGB and any photographic metadata from the same EXR part."""
    with OpenEXR.File(str(path), separate_channels=True) as image:
        if len(image.parts) != 1:
            raise ValueError("Select a single-part RGB EXR master")
        header = image.header()
        space = header.get("colorInteropID", "unknown")
        if space not in {"lin_rec709_scene", "unknown"} or header.get("acesImageContainerFlag", 0):
            raise ValueError("The EXR declares a different color space; expected scene-linear Rec.709/D65")
        primaries = header.get("chromaticities")
        if primaries is not None:
            if not np.allclose(primaries, REC709, rtol=0, atol=0.0001):
                raise ValueError("The EXR chromaticities differ from Rec.709/D65")
        elif space == "unknown" and not assume_rec709:
            raise ValueError("The EXR is untagged; pass --assume-rec709 for known scene-linear Rec.709/D65")
        channels = image.channels()
        planes = []
        for name in "RGB":
            channel = channels.get(name)
            if channel is None or channel.xSampling != 1 or channel.ySampling != 1:
                raise ValueError("The EXR needs full-resolution R, G and B channels")
            plane = channel.pixels
            if plane.ndim != 2 or plane.dtype not in (np.float16, np.float32):
                raise ValueError("The EXR needs flat half- or single-precision RGB samples")
            if not np.isfinite(plane).all():
                raise ValueError(f"Channel {name} contains non-finite samples")
            planes.append(plane)
        return np.stack(planes, axis=-1, dtype=np.float32), capture_exif(header)


def read_linear_exr(path: Path, *, assume_rec709=False) -> np.ndarray:
    """Read stored scene RGB; the scene capture's spare alpha channel is ignored."""
    return read_photo_exr(path, assume_rec709=assume_rec709)[0]


def rgb_to_xyz(chromaticities):
    xy = np.asarray(chromaticities).reshape(4, 2)
    xyz = np.column_stack((xy, 1 - xy.sum(axis=1))) / xy[:, 1:2]
    primaries = xyz[:3].T
    return primaries * np.linalg.solve(primaries, xyz[3])


# ARRI, ALEXA Log C Curve – Usage in VFX (2017), pp. 9–10, exposure values, EI 800.
# https://www.arri.com/resource/blob/31918/66f56e6abb6e5b6553929edf9aa7483e/2017-03-alexa-logc-curve-in-vfx-data.pdf
# Fujifilm, F-Log Data Sheet v1.2, pp. 2–3, scene reflection and F-Gamut.
# https://dl.fujifilm-x.com/technical-data/F-Log_DataSheet_E_Ver.1.2.pdf
MATRICES = {name: np.linalg.solve(rgb_to_xyz(gamut), rgb_to_xyz(REC709))
            for name, gamut in (("logc3", AWG3), ("flog", F_GAMUT))}


def encode(linear, profile):
    """Encode destination-gamut RGB. Signed values use the linear toe."""
    if profile == "logc3":
        cut, a, b, c, d, e, f = 0.010591, 5.555556, 0.052272, 0.247190, 0.385537, 5.367655, 0.092809
        use_log = linear > cut
    elif profile == "flog":
        cut, a, b, c, d, e, f = 0.00089, 0.555556, 0.009468, 0.344676, 0.790453, 8.735631, 0.092864
        use_log = linear >= cut
    else:
        raise ValueError(f"Unknown log profile: {profile}")
    encoded = e * linear + f
    encoded[use_log] = c * np.log10(a * linear[use_log] + b) + d
    return encoded


def gamut_blocks(rgb, profile):
    if profile not in PROFILES:
        raise ValueError(f"Unknown log profile: {profile}")
    if rgb.ndim != 3 or rgb.shape[-1] != 3 or min(rgb.shape) < 1:
        raise ValueError("Expected a nonempty RGB image")
    for first in range(0, len(rgb), 64):
        converted = rgb[first:first + 64] @ MATRICES[profile].T
        if not np.isfinite(converted).all():
            raise ValueError("Invalid linear sample after gamut conversion")
        yield first, converted


def highlight_exposure(rgb: np.ndarray, profile: str) -> int:
    """Find the closest whole-stop offset to zero that retains the brightest highlight."""
    peak = max(float(block.max()) for _, block in gamut_blocks(rgb, profile))
    stops = 0
    # Match the exact upper bound used by the writer, including rounding at
    # the curve endpoint. Exposure doubling/halving is exact in binary.
    while encode(np.array([peak]), profile)[0] > 1:
        peak *= 0.5
        stops -= 1
    return stops


def publish_new_file(staged: Path, destination: Path):
    """Publish the finished file without replacing another export."""
    if os.name == "nt":
        # Windows rename fails if a peer has already published the destination.
        # It also works on local volumes that do not implement hard links.
        os.rename(staged, destination)
    else:
        os.link(staged, destination)


def _capture_directory(tags, order, offset, count, entry, pointer):
    # Each supported photograph field is one SHORT or one unsigned RATIONAL.
    # Classic TIFF stores rationals after the directory. In BigTIFF each
    # entry has room for all eight bytes, including with older Pillow versions.
    fields = [(int(ExifTags.Base.ExifVersion), TiffTags.UNDEFINED, 4, b"0232")]
    for tag, value in tags.items():
        if isinstance(value, IFDRational):
            data = struct.pack(order + "II", value.numerator, value.denominator)
            fields.append((tag, TiffTags.RATIONAL, 1, data))
        else:
            fields.append((tag, TiffTags.SHORT, 1, struct.pack(order + "H", value)))
    entries = bytearray(count.pack(len(fields)))
    values = bytearray()
    start = offset + count.size + entry.size * len(fields) + pointer.size
    for tag, dtype, length, data in sorted(fields):
        if len(data) > pointer.size:
            address = start + len(values)
            if address >= 1 << (8 * pointer.size):
                raise ValueError("EXIF directory exceeds the TIFF offset limit")
            values.extend(data)
            data = pointer.pack(address)
        entries.extend(entry.pack(tag, dtype, length, data))
    return entries + pointer.pack(0) + values


def write_tiff_exif(path, tags):
    """Install the photographic ExifIFD in our newly authored TIFF, before publication."""
    # OpenImageIO's TIFF writer explicitly skips PhotographicSensitivity (34855).
    # It reserves an ExifIFD pointer for ExifVersion. Append the complete directory
    # and redirect only that pointer, preserving the finished compressed strips.
    with path.open("r+b") as stream:
        header = stream.read(16)
        order = {"II": "<", "MM": ">"}[header[:2].decode("ascii")]
        big = struct.unpack_from(order + "H", header, 2)[0] == 43
        entry = struct.Struct(order + ("HHQ8s" if big else "HHI4s"))
        count_size = struct.Struct(order + ("Q" if big else "H"))
        pointer = struct.Struct(order + ("Q" if big else "I"))
        stream.seek(pointer.unpack_from(header, 8 if big else 4)[0])
        count, = count_size.unpack(stream.read(count_size.size))
        for _ in range(count):
            tag, dtype, length, _ = entry.unpack(stream.read(entry.size))
            if tag == ExifTags.IFD.Exif and length == 1:
                break
        else:
            raise ValueError("The TIFF writer did not reserve an EXIF directory")
        location = stream.tell() - pointer.size
        # A BigTIFF may use either 32- or 64-bit offsets for a tag.
        address = struct.Struct(order + {4: "I", 13: "I", 16: "Q", 18: "Q"}[dtype])
        end = stream.seek(0, os.SEEK_END)
        align = 8 if big else 2
        offset = end + (-end % align)
        if offset >= 1 << (8 * address.size):
            raise ValueError("EXIF directory exceeds the TIFF offset limit")
        directory = _capture_directory(tags, order, offset, count_size, entry, pointer)
        if offset + len(directory) >= 1 << (8 * pointer.size):
            raise ValueError("EXIF directory exceeds the TIFF offset limit")
        stream.write(bytes(offset - end))
        stream.write(directory)
        stream.seek(location)
        stream.write(address.pack(offset))


def write_log_tiff(output: Path, rgb: np.ndarray, profile: str, *, exposure=0.0, exif=None):
    """Publish one complete TIFF without replacing another photo. Return clipped sample counts."""
    import OpenImageIO as oiio

    output = Path(output)
    if output.suffix.lower() not in (".tif", ".tiff"):
        raise ValueError("Use .tif or .tiff for the output")
    if output.exists():
        raise FileExistsError(f"Output exists: {output}")
    if profile not in PROFILES:
        raise ValueError(f"Unknown log profile: {profile}")
    if rgb.ndim != 3 or rgb.shape[-1] != 3 or min(rgb.shape) < 1:
        raise ValueError("Expected a nonempty RGB image")
    if not math.isfinite(exposure):
        raise ValueError("Exposure offset must be finite")
    gain = 2.0 ** exposure
    if not math.isfinite(gain) or gain == 0:
        raise ValueError("Exposure offset is outside the representable range")
    height, width = rgb.shape[:2]
    spec = oiio.ImageSpec(width, height, 3, oiio.UINT16)
    spec.attribute("compression", "zip")
    spec.attribute("Software", "War in the North Photo Camera")
    if exif:
        spec.attribute("Exif:ExifVersion", "0232")
    spec.attribute("ImageDescription", f"{PROFILES[profile]}. Full range 0-65535. "
                   f"Source scene-linear Rec.709/D65. Exposure offset {exposure:g} stops.")
    low, high = 0, 0
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".witn-grade-") as work:
        temp = Path(work) / "photo.tiff"
        writer = oiio.ImageOutput.create(str(temp))
        if writer is None or not writer.open(str(temp), spec):
            raise OSError(oiio.geterror() if writer is None else writer.geterror())
        try:
            for first, converted in gamut_blocks(rgb, profile):
                converted *= gain
                if not np.isfinite(converted).all():
                    raise ValueError("Invalid linear sample after applying exposure and gamut conversion")
                encoded = encode(converted, profile)
                low += int(np.count_nonzero(encoded < 0))
                high += int(np.count_nonzero(encoded > 1))
                codes = np.rint(np.clip(encoded, 0, 1) * 65535).astype(np.uint16)
                if not writer.write_scanlines(first, first + len(codes), 0, codes):
                    raise OSError(writer.geterror())
        finally:
            if not writer.close():
                raise OSError(writer.geterror())
        if exif:
            write_tiff_exif(temp, exif)
        publish_new_file(temp, output)
    return low, high


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exr", type=Path)
    parser.add_argument("--profile", choices=PROFILES, default="logc3")
    parser.add_argument("-o", "--output", type=Path, help="Default: beside the source EXR")
    parser.add_argument("--assume-rec709", action="store_true",
                        help="Use known scene-linear Rec.709/D65 for an untagged EXR")
    parser.add_argument("--ev", type=float, help="Set an exposure offset in stops (default: fit highlights)")
    args = parser.parse_args()
    output = args.output or args.exr.with_name(f"{args.exr.stem}_{args.profile}.tiff")
    try:
        rgb, exif = read_photo_exr(args.exr, assume_rec709=args.assume_rec709)
        exposure = highlight_exposure(rgb, args.profile) if args.ev is None else args.ev
        low, high = write_log_tiff(output, rgb, args.profile, exposure=exposure, exif=exif)
    except (OSError, ValueError, RuntimeError, OverflowError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Saved {output} ({PROFILES[args.profile]}, full range)")
    print(f"Log exposure offset {exposure:g} EV. To match the EXR, apply {-exposure:g} EV "
          "after decoding to scene-linear.")
    print(f"Clipped RGB channel samples: {low} below black, {high} above white (of {rgb.size})")


if __name__ == "__main__":
    main()
