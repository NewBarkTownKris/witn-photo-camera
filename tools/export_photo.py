"""Develop the missing companions for War in the North scene-linear EXRs."""

import argparse
import configparser
import os
from pathlib import Path, PureWindowsPath
import re
import sys


APP_ID = "2523770"


def proton_prefix():
    compat = os.environ.get("STEAM_COMPAT_DATA_PATH")
    if compat and (Path(compat).name == APP_ID or os.environ.get("SteamAppId") == APP_ID):
        return Path(compat) / "pfx"
    home = Path.home()
    data_home = Path(os.environ.get("XDG_DATA_HOME") or home / ".local/share")
    roots = [Path(p) for p in os.environ.get("STEAM_COMPAT_LIBRARY_PATHS", "").split(os.pathsep) if p]
    client = os.environ.get("STEAM_COMPAT_CLIENT_INSTALL_PATH")
    if client:
        roots.append(Path(client))
    roots.extend((home / ".steam/steam", data_home / "Steam",
                  home / ".var/app/com.valvesoftware.Steam/data/Steam"))
    libraries = list(roots)
    for root in roots:
        try:
            vdf = (root / "steamapps/libraryfolders.vdf").read_text(encoding="utf-8-sig")
        except (OSError, UnicodeError):
            continue
        for match in re.finditer(r'^\s*"path"\s*"((?:\\.|[^"\\])*)"', vdf, re.MULTILINE):
            library = Path(re.sub(r'\\([\\"])', r'\1', match.group(1)))
            if library.is_absolute():
                libraries.append(library)
    for root in libraries:
        prefix = root / "steamapps/compatdata" / APP_ID / "pfx"
        if prefix.is_dir():
            return prefix
    raise FileNotFoundError("War in the North Proton profile was not found. Pass --directory to select your photos.")


def selected_directory():
    prefix = None
    if os.name == "nt":
        local = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData/Local")
    else:
        prefix = proton_prefix()
        local = prefix / "drive_c/users/steamuser/AppData/Local"
    camera = local / "WITNPhotoCamera"
    try:
        raw = (camera / "settings.ini").read_bytes()
    except FileNotFoundError:
        return camera / "Photos"
    if raw.startswith((b"\xfe\xff", b"\xff\xfe")):
        data = raw.decode("utf-16")
    else:
        try:
            data = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            data = raw.decode("mbcs" if os.name == "nt" else "cp1252")
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        settings.read_string(data)
    except configparser.Error as error:
        raise ValueError(f"Could not read camera settings: {error}") from error
    output = next((section for section in settings.sections() if section.lower() == "output"), None)
    value = settings.get(output, "Directory", fallback="").strip().strip('"') if output else ""
    if not value:
        return camera / "Photos"
    location = PureWindowsPath(value)
    if not location.is_absolute():
        raise ValueError(f"The configured photo directory must be an absolute Windows path: {value}")
    if prefix is None:
        return Path(value)
    if location.drive.startswith("\\\\"):
        network = prefix / "dosdevices/unc" / Path(*location.drive.lstrip("\\").split("\\"))
        return network.joinpath(*location.parts[1:])
    drive = prefix / "dosdevices" / location.drive.lower()
    if not drive.is_dir():
        raise FileNotFoundError(f"Wine drive {location.drive} is unavailable. Use --directory to supply the host path.")
    return drive.resolve().joinpath(*location.parts[1:])


def discover_shots(directory):
    return sorted(path for path in directory.iterdir()
                  if path.suffix.lower() == ".exr" and not path.name.startswith(".")
                  and not path.name.lower().endswith(".source.exr") and path.is_file())


def develop(source, formats, args):
    pending = {}
    for fmt in formats:
        output = (source.with_suffix(".dng") if fmt == "dng"
                  else source.with_name(f"{source.stem}_{fmt}.tiff"))
        if not output.is_file():
            pending[fmt] = output

    kept = len(formats) - len(pending)
    if not pending:
        return 0, kept, False
    try:
        from grade import read_photo_exr
        rgb, exif = read_photo_exr(source)
    except (OSError, ValueError, RuntimeError, ImportError) as error:
        print(f"{source}: {error}", file=sys.stderr)
        return 0, kept, True

    saved = 0
    failed = False
    for fmt, path in pending.items():
        try:
            if fmt == "dng":
                from linear_dng import write_dng
                stops, negative = write_dng(rgb, path, clip_negative=args.clip_negative, integer=args.integer_dng,
                                            exif=exif)
                kind = "16-bit integer" if args.integer_dng else "32-bit floating point"
                detail = f"{kind}, baseline exposure +{stops} EV, {negative} negative channel samples floored"
            else:
                from grade import highlight_exposure, write_log_tiff
                ev = args.ev if args.ev is not None else highlight_exposure(rgb, fmt)
                low, high = write_log_tiff(path, rgb, fmt, exposure=ev, exif=exif)
                detail = (f"{ev:+g} EV encoded, {-ev:+g} EV restores the scene after log decoding, "
                          f"{low} low and {high} high channel samples clipped")
            print(f"Saved {path} ({detail})", flush=True)
            saved += 1
        except (OSError, ValueError, RuntimeError, ImportError, OverflowError) as error:
            # Another exporter may publish between our directory check and its atomic link.
            if isinstance(error, FileExistsError) and path.is_file():
                kept += 1
            else:
                print(f"{source} ({fmt}): {error}", file=sys.stderr)
                failed = True
    return saved, kept, failed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument("photo", nargs="?", type=Path, help="EXR or directory (default: camera screenshot directory)")
    inputs.add_argument("--directory", type=Path, help="Scan this directory for missing companions")
    parser.add_argument("--format", choices=("all", "dng", "logc3", "flog"), default="all")
    parser.add_argument("--ev", type=float, help="Set a log TIFF exposure in stops (default: fit highlights)")
    parser.add_argument("--integer-dng", action="store_true", help="Write a 16-bit integer DNG for legacy readers")
    parser.add_argument("--clip-negative", action="store_true", help="Floor negative RGB for the DNG companion")
    args = parser.parse_args(argv)
    formats = ("logc3", "flog", "dng") if args.format == "all" else (args.format,)
    try:
        if args.photo is not None and not args.photo.is_dir():
            if args.photo.suffix.lower() != ".exr" or not args.photo.is_file():
                parser.error(f"Choose an existing scene-linear EXR or directory: {args.photo}")
            sources = [args.photo]
        else:
            directory = args.directory or args.photo
            if directory is None:
                directory = selected_directory()
            sources = discover_shots(directory)
            print(f"Scanning {directory}", flush=True)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")

    saved = kept = failed = 0
    for source in sources:
        written, existing, problem = develop(source, formats, args)
        saved += written
        kept += existing
        failed += int(problem)
    print(f"Checked {len(sources)} EXR(s). Saved {saved} companion(s), kept {kept} existing. "
          f"Failed shots: {failed}.")
    return int(failed > 0)


if __name__ == "__main__":
    sys.exit(main())
