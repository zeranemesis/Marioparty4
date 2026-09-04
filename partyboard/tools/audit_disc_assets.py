#!/usr/bin/env python3
"""Audit Mario Party 4 data archives directly from an uncompressed GCM/ISO.

This intentionally reads metadata only: no copyrighted game asset is extracted.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class DiscFile:
    path: str
    offset: int
    size: int


def read_u32_be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def parse_fst(iso_path: Path) -> tuple[str, int, int, int, dict[str, DiscFile]]:
    with iso_path.open("rb") as iso:
        header = iso.read(0x42C)
        if len(header) < 0x42C:
            raise ValueError("Image too short to contain a GameCube disc header")
        game_id = header[:6].decode("ascii", errors="replace")
        revision = header[7]
        fst_offset = read_u32_be(header, 0x424)
        fst_size = read_u32_be(header, 0x428)
        if fst_offset <= 0 or fst_size < 12:
            raise ValueError("Invalid FST location in disc header")
        iso.seek(fst_offset)
        fst = iso.read(fst_size)

    if len(fst) != fst_size:
        raise ValueError("Truncated FST")
    entry_count = read_u32_be(fst, 8)
    table_size = entry_count * 12
    if entry_count == 0 or table_size > len(fst):
        raise ValueError("Invalid FST entry table")
    strings = fst[table_size:]

    def name_at(name_offset: int) -> str:
        if name_offset >= len(strings):
            raise ValueError(f"FST name offset outside string table: {name_offset}")
        end = strings.find(b"\0", name_offset)
        if end < 0:
            raise ValueError("Unterminated FST name")
        return strings[name_offset:end].decode("shift_jis", errors="replace")

    files: dict[str, DiscFile] = {}

    def walk(directory_index: int, prefix: str) -> int:
        directory_word = read_u32_be(fst, directory_index * 12)
        if directory_word >> 24 == 0:
            raise ValueError("FST traversal expected a directory")
        next_index = read_u32_be(fst, directory_index * 12 + 8)
        index = directory_index + 1
        while index < next_index:
            base = index * 12
            word = read_u32_be(fst, base)
            name = name_at(word & 0x00FFFFFF)
            path = f"{prefix}/{name}" if prefix else name
            if word >> 24:
                index = walk(index, path)
            else:
                offset = read_u32_be(fst, base + 4)
                size = read_u32_be(fst, base + 8)
                files[path.lower()] = DiscFile(path, offset, size)
                index += 1
        return next_index

    walk(0, "")
    return game_id, revision, fst_offset, fst_size, files


def expected_archives(repo: Path) -> dict[str, str]:
    table = (repo / "include" / "datadir_table.h").read_text(encoding="utf-8")
    pattern = re.compile(
        r'DATADIR\(\s*([A-Z0-9_]+)\s*,\s*DATADIR_PREFIX\s*"/([^"]+\.bin)"\s*\)'
    )
    return {symbol: f"data/{filename}" for symbol, filename in pattern.findall(table)}


def referenced_indices(repo: Path) -> dict[str, set[int]]:
    pattern = re.compile(
        r"DATA_MAKE_NUM\(\s*DATADIR_([A-Z0-9_]+)\s*,\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*\)"
    )
    result: dict[str, set[int]] = {}
    for source in (repo / "src").rglob("*"):
        if not source.is_file() or source.suffix.lower() not in {".c", ".cpp", ".h", ".hpp"}:
            continue
        text = source.read_text(encoding="utf-8", errors="ignore")
        for symbol, literal in pattern.findall(text):
            result.setdefault(symbol, set()).add(int(literal, 0))
    return result


def audit_archive(iso, disc_file: DiscFile) -> tuple[int | None, list[str]]:
    errors: list[str] = []
    if disc_file.size < 4:
        return None, ["archive shorter than its header"]
    iso.seek(disc_file.offset)
    count_raw = iso.read(4)
    if len(count_raw) != 4:
        return None, ["archive header could not be read"]
    count = read_u32_be(count_raw, 0)
    # setup.bin is a four-byte, zero-entry placeholder in this retail revision.
    if count == 0 and disc_file.size == 4:
        return 0, []
    table_size = 4 + count * 4
    if count == 0 or count > 10000:
        return count, [f"implausible file count: {count}"]
    if table_size > disc_file.size:
        return count, ["offset table extends beyond archive"]

    iso.seek(disc_file.offset + 4)
    raw_offsets = iso.read(count * 4)
    if len(raw_offsets) != count * 4:
        return count, ["truncated offset table"]
    offsets = list(struct.unpack(f">{count}I", raw_offsets))
    previous = -1
    for index, offset in enumerate(offsets):
        if offset < table_size or offset + 8 > disc_file.size:
            errors.append(f"entry {index}: invalid offset 0x{offset:X}")
        if offset < previous:
            errors.append(f"entry {index}: offsets are not monotonic")
        previous = offset
    if errors:
        return count, errors[:20]

    # Validate each embedded entry header without decoding or extracting it.
    for index, offset in enumerate(offsets):
        next_offset = offsets[index + 1] if index + 1 < count else disc_file.size
        iso.seek(disc_file.offset + offset)
        entry_header = iso.read(8)
        if len(entry_header) != 8:
            errors.append(f"entry {index}: truncated header")
            continue
        raw_size, decode_type = struct.unpack(">II", entry_header)
        if raw_size == 0:
            errors.append(f"entry {index}: zero decoded size")
        # See include/game/data.h: NONE, LZ, SLIDE, FSLIDE_ALT, FSLIDE, RLE.
        if decode_type > 5:
            errors.append(f"entry {index}: unexpected decode type {decode_type}")
        if next_offset < offset + 8:
            errors.append(f"entry {index}: overlaps the next entry")
    return count, errors[:20]


def sha256_region(path: Path, disc_file: DiscFile) -> str:
    digest = hashlib.sha256()
    remaining = disc_file.size
    with path.open("rb") as iso:
        iso.seek(disc_file.offset)
        while remaining:
            block = iso.read(min(1024 * 1024, remaining))
            if not block:
                raise ValueError(f"Truncated file data for {disc_file.path}")
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def render_report(
    iso_path: Path,
    game_id: str,
    revision: int,
    fst_offset: int,
    fst_size: int,
    files: dict[str, DiscFile],
    archives: dict[str, str],
    references: dict[str, set[int]],
    results: dict[str, tuple[int | None, list[str]]],
) -> str:
    missing = [(symbol, path) for symbol, path in archives.items() if path.lower() not in files]
    malformed = [(symbol, errors) for symbol, (_, errors) in results.items() if errors]
    insufficient: list[tuple[str, int, int]] = []
    for symbol, indices in references.items():
        if symbol not in results or not indices:
            continue
        count = results[symbol][0]
        if count is not None and max(indices) >= count:
            insufficient.append((symbol, count, max(indices)))

    m406_file = files.get(archives["M406"].lower())
    m406_count, m406_errors = results.get("M406", (None, ["not audited"]))
    m406_max = max(references.get("M406", {-1}))
    status = "OK" if not missing and not malformed else "ANOMALIES"
    empty = [
        (symbol, path)
        for symbol, path in archives.items()
        if symbol in results and results[symbol][0] == 0 and not results[symbol][1]
    ]

    lines = [
        "# Audit des assets de Mario Party 4",
        "",
        f"- Image : `{iso_path}`",
        f"- Identifiant disque : `{game_id}`",
        f"- Révision du disque : **{revision}**",
        f"- FST : offset `0x{fst_offset:X}`, taille `{fst_size}` octets, `{len(files)}` fichiers",
        f"- Conteneurs attendus par le code : **{len(archives)}**",
        f"- Conteneurs trouvés : **{len(archives) - len(missing)}**",
        f"- Résultat structurel : **{status}**",
        "",
        "## Avalanche! (`m406.bin`)",
        "",
    ]
    if m406_file is None:
        lines.append("- Le conteneur est absent du disque.")
    else:
        lines.extend(
            [
                f"- Taille : `{m406_file.size}` octets",
                f"- SHA-256 : `{sha256_region(iso_path, m406_file)}`",
                f"- Entrées présentes : **{m406_count}**",
                f"- Plus grand index constant utilisé par le code : **{m406_max}**",
                f"- Couverture des indices : **{'OK' if m406_count is not None and m406_max < m406_count else 'INSUFFISANTE'}**",
                f"- Structure interne : **{'OK' if not m406_errors else 'ANOMALIES'}**",
            ]
        )
        for error in m406_errors:
            lines.append(f"  - {error}")

    lines.extend(["", "## Contrôles globaux", ""])
    if not missing:
        lines.append("- Aucun conteneur `data/*.bin` attendu par le code ne manque.")
    else:
        lines.append(f"- Conteneurs manquants : **{len(missing)}**")
        for symbol, path in missing:
            lines.append(f"  - `{symbol}` : `{path}`")
    if not malformed:
        lines.append("- Tous les conteneurs présents ont une table et des en-têtes d’entrées cohérents.")
    else:
        lines.append(f"- Conteneurs structurellement suspects : **{len(malformed)}**")
        for symbol, errors in malformed:
            lines.append(f"  - `{symbol}` : {'; '.join(errors)}")
    if empty:
        lines.append(
            "- Conteneurs vides présents sur cette révision (emplacements réservés) : "
            + ", ".join(f"`{path}`" for _, path in empty)
            + "."
        )
    if not insufficient:
        lines.append("- Tous les indices constants repérés dans le code existent dans leurs conteneurs.")
    else:
        lines.append(
            "- Avertissements de compatibilité source (références constantes appartenant notamment à d’autres chemins régionaux ou écrans) :"
        )
        for symbol, count, maximum in insufficient:
            lines.append(f"  - `{symbol}` : {count} entrées, index {maximum} demandé")

    lines.extend(
        [
            "",
            "## Portée du contrôle",
            "",
            "L’audit vérifie la présence, les limites, les tables d’offsets et les en-têtes des assets, sans les extraire. Les références hors limites globales sont signalées à titre informatif, car le dépôt contient aussi des chemins régionaux ou alternatifs qui ne sont pas tous exécutés avec cette image. Il ne prouve pas à lui seul que le moteur PC interprète chaque modèle ou effet exactement comme la GameCube.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path, help="Uncompressed GameCube ISO/GCM")
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="Party Board source tree")
    parser.add_argument("--report", type=Path, help="Write a Markdown report")
    args = parser.parse_args()

    iso_path = args.iso.resolve()
    repo = args.repo.resolve()
    game_id, revision, fst_offset, fst_size, files = parse_fst(iso_path)
    archives = expected_archives(repo)
    references = referenced_indices(repo)
    results: dict[str, tuple[int | None, list[str]]] = {}
    with iso_path.open("rb") as iso:
        for symbol, path in archives.items():
            disc_file = files.get(path.lower())
            if disc_file is not None:
                results[symbol] = audit_archive(iso, disc_file)

    report = render_report(
        iso_path, game_id, revision, fst_offset, fst_size, files, archives, references, results
    )
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(report, encoding="utf-8")
    print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
