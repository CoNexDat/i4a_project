#!/usr/bin/env python3
"""Copy every image referenced by an ESP-IDF flash_args into a portable bundle."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil

from esp_rpi_flasher import FlasherError, parse_flash_args_tokens


def export_bundle(build_dir: Path, output_dir: Path) -> None:
    build_dir = build_dir.resolve()
    output_dir = output_dir.resolve()
    args_path = build_dir / "flash_args"
    if not args_path.is_file():
        raise FlasherError(f"No se encontro {args_path}; primero ejecuta idf.py build")
    before, options, segments = parse_flash_args_tokens(args_path)
    if before:
        raise FlasherError("Usa el flash_args generado por ESP-IDF, sin comando esptool")

    images = []
    for offset, image in segments:
        try:
            relative = image.relative_to(build_dir)
        except ValueError as exc:
            raise FlasherError(f"La imagen esta fuera de build: {image}") from exc
        if not image.is_file():
            raise FlasherError(f"Falta la imagen {image}")
        images.append((offset, image, relative))

    # Validate all inputs before creating the destination; never overwrite a bundle.
    if output_dir.exists():
        raise FlasherError(f"El destino ya existe: {output_dir}; usa una carpeta nueva")
    output_dir.mkdir(parents=True)
    manifest = []
    for offset, image, relative in images:
        destination = output_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, destination)
        manifest.append({"offset": offset, "path": relative.as_posix(),
                         "sha256": hashlib.sha256(destination.read_bytes()).hexdigest()})
    shutil.copyfile(args_path, output_dir / "flash_args")
    shutil.copyfile(Path(__file__).with_name("esp_rpi_flasher.py"),
                    output_dir / "esp_rpi_flasher.py")
    (output_dir / "manifest.json").write_text(
        json.dumps({"flash_options": options, "images": manifest}, indent=2) + "\n",
        encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("app/build"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        export_bundle(args.build_dir, args.output)
    except (FlasherError, OSError) as exc:
        parser.exit(2, f"Error: {exc}\n")
    print(f"Bundle exportado a {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
