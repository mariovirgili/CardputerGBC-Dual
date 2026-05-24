#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterable


HANDLER_RE = re.compile(r"m68k_op_[A-Za-z0-9_]+")
DEFAULT_TABLE = Path("src/genesis/gwenesis/cpus/M68K/m68ki_instruction_jump_table.h")
DEFAULT_FULL_TABLE = Path("src/genesis/gwenesis/cpus/M68K/m68ki_instruction_jump_table_full.h")
POINTER_SIZE = 4


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze the generated Gwenesis M68K dispatch table and estimate "
            "compression opportunities for shared subtable layouts."
        )
    )
    parser.add_argument(
        "table",
        nargs="?",
        type=Path,
        default=None,
        help="Path to the generated jump table header.",
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="Analyze m68ki_instruction_jump_table_full.h by default.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON instead of text.",
    )
    parser.add_argument(
        "--emit-c-header",
        type=Path,
        default=None,
        help=(
            "Write a generated C header for the top-byte compressed dispatch "
            "proposal."
        ),
    )
    parser.add_argument(
        "--emit-hybrid-c-header",
        type=Path,
        default=None,
        help=(
            "Write a generated C header for a hybrid dispatch proposal with "
            "direct subtables for selected hot top-byte groups and compressed "
            "dispatch for the rest."
        ),
    )
    parser.add_argument(
        "--hybrid-hot-bytes",
        default="66,4A,30,67,4E",
        help=(
            "Comma-separated list of hot top-byte opcode groups to keep as "
            "direct 256-entry tables in the hybrid proposal."
        ),
    )
    return parser.parse_args()


def resolve_table_path(args: argparse.Namespace) -> Path:
    if args.table is not None:
        return args.table
    return DEFAULT_FULL_TABLE if args.full else DEFAULT_TABLE


def load_handlers(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    handlers = HANDLER_RE.findall(text)
    if not handlers:
        raise ValueError(f"No opcode handlers found in {path}")
    return handlers


def chunked(seq: list[str], size: int) -> Iterable[list[str]]:
    for i in range(0, len(seq), size):
        yield seq[i : i + size]


def digest_block(block: list[str]) -> str:
    joined = "\n".join(block).encode("utf-8")
    return hashlib.sha1(joined).hexdigest()[:12]


def compression_stats(handlers: list[str], block_size: int) -> dict:
    blocks = list(chunked(handlers, block_size))
    block_map: dict[tuple[str, ...], int] = {}
    block_index: list[int] = []
    repeated_examples: defaultdict[int, list[int]] = defaultdict(list)

    for i, block in enumerate(blocks):
        key = tuple(block)
        idx = block_map.setdefault(key, len(block_map))
        block_index.append(idx)
        if len(repeated_examples[idx]) < 4:
            repeated_examples[idx].append(i)

    unique_blocks = [None] * len(block_map)
    for key, idx in block_map.items():
        unique_blocks[idx] = list(key)

    index_bits = max(1, math.ceil(math.log2(max(1, len(unique_blocks)))))
    index_bytes = 1 if index_bits <= 8 else 2 if index_bits <= 16 else 4
    dense_table_bytes = len(handlers) * POINTER_SIZE
    shared_subtables_bytes = len(unique_blocks) * block_size * POINTER_SIZE
    l1_index_bytes = len(blocks) * index_bytes
    l1_pointer_bytes = len(blocks) * POINTER_SIZE

    repeat_counts = Counter(block_index)
    top_layouts = []
    for idx, count in repeat_counts.most_common(8):
        block = unique_blocks[idx]
        top_layouts.append(
            {
                "layout_index": idx,
                "count": count,
                "digest": digest_block(block),
                "all_illegal": all(handler == "m68k_op_illegal" for handler in block),
                "top_handlers": Counter(block).most_common(4),
                "example_block_indices": repeated_examples[idx],
            }
        )

    return {
        "block_size": block_size,
        "block_index": block_index,
        "total_blocks": len(blocks),
        "unique_blocks": len(unique_blocks),
        "unique_block_data": unique_blocks,
        "reused_blocks": sum(1 for count in repeat_counts.values() if count > 1),
        "dense_table_bytes": dense_table_bytes,
        "shared_subtables_bytes": shared_subtables_bytes,
        "l1_index_bytes": l1_index_bytes,
        "l1_pointer_bytes": l1_pointer_bytes,
        "estimated_total_bytes_indexed": shared_subtables_bytes + l1_index_bytes,
        "estimated_total_bytes_pointer_l1": shared_subtables_bytes + l1_pointer_bytes,
        "top_layouts": top_layouts,
    }


def top_group_summary(handlers: list[str], group_size: int, label_width: int) -> list[dict]:
    groups = []
    for group_idx, block in enumerate(chunked(handlers, group_size)):
        illegal = sum(1 for handler in block if handler == "m68k_op_illegal")
        groups.append(
            {
                "group": f"{group_idx:0{label_width}X}",
                "entries": len(block),
                "illegal": illegal,
                "illegal_pct": round((illegal * 100.0) / len(block), 2),
                "unique_handlers": len(set(block)),
                "top_handlers": Counter(block).most_common(5),
            }
        )
    return groups


def build_report(path: Path, handlers: list[str]) -> dict:
    total = len(handlers)
    illegal = sum(1 for handler in handlers if handler == "m68k_op_illegal")
    unique_handlers = len(set(handlers))
    handler_counts = Counter(handlers)

    report = {
        "table": str(path),
        "total_entries": total,
        "illegal_entries": illegal,
        "illegal_pct": round((illegal * 100.0) / total, 2),
        "unique_handlers": unique_handlers,
        "top_handlers": handler_counts.most_common(20),
        "compression": {
            "top_byte": compression_stats(handlers, 256),
            "top_nibble": compression_stats(handlers, 4096),
        },
        "groups": {
            "top_byte": top_group_summary(handlers, 256, 2),
            "top_nibble": top_group_summary(handlers, 4096, 1),
        },
    }
    return report


def c_integer_type(max_value: int) -> str:
    if max_value <= 0xFF:
        return "uint8_t"
    if max_value <= 0xFFFF:
        return "uint16_t"
    return "uint32_t"


def sanitize_guard_token(path: Path) -> str:
    token = re.sub(r"[^A-Za-z0-9]+", "_", path.stem.upper())
    return f"{token}_GENERATED_H"


def format_c_rows(values: list[str], row_width: int = 8, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(values), row_width):
        chunk = values[i : i + row_width]
        lines.append(indent + ", ".join(chunk))
    return ",\n".join(lines)


def emit_top_byte_header(report: dict, output_path: Path) -> None:
    stats = report["compression"]["top_byte"]
    block_index: list[int] = stats["block_index"]
    unique_blocks: list[list[str]] = stats["unique_block_data"]
    index_type = c_integer_type(max(block_index) if block_index else 0)
    guard = sanitize_guard_token(output_path)
    table_name = output_path.stem

    l1_values = [str(value) for value in block_index]
    subtables_rows = []
    for block in unique_blocks:
        subtables_rows.append(
            "    {\n"
            + format_c_rows(block, row_width=8, indent="      ")
            + "\n    }"
        )
    subtables_blob = ",\n".join(subtables_rows)

    content = f"""// Auto-generated by scripts/analyze_md_dispatch_table.py
// Source table: {report["table"]}
// Dense entries: {report["total_entries"]}
// Top-byte blocks: {stats["total_blocks"]} -> unique subtables: {stats["unique_blocks"]}
//
// Dispatch shape:
//   hi = opcode >> 8
//   lo = opcode & 0xff
//   handler = {table_name}_subtables[{table_name}_l1[hi]][lo]
//
// Note:
//   This proposal preserves the exact per-opcode handler mapping while deduplicating
//   repeated 256-entry top-byte subtables. If hi is outside the generated L1 range,
//   the helper falls back to m68k_op_illegal().

#ifndef {guard}
#define {guard}

#include <stdint.h>

#define {table_name.upper()}_L1_COUNT {stats["total_blocks"]}
#define {table_name.upper()}_SUBTABLE_COUNT {stats["unique_blocks"]}
#define {table_name.upper()}_SUBTABLE_WIDTH 256

static const {index_type} {table_name}_l1[{stats["total_blocks"]}] =
{{
{format_c_rows(l1_values, row_width=16, indent="    ")}
}};

static void (* const {table_name}_subtables[{stats["unique_blocks"]}][256])(void) =
{{
{subtables_blob}
}};

static inline void (*{table_name}_resolve(uint16_t opcode))(void)
{{
    const unsigned hi = (unsigned)(opcode >> 8);
    if (hi >= {table_name.upper()}_L1_COUNT) {{
        return m68k_op_illegal;
    }}
    return {table_name}_subtables[{table_name}_l1[hi]][opcode & 0xff];
}}

static inline void {table_name}_dispatch(uint16_t opcode)
{{
    {table_name}_resolve(opcode)();
}}

#endif /* {guard} */
"""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(content, encoding="utf-8")


def parse_hot_bytes(spec: str) -> list[int]:
    values: list[int] = []
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        values.append(int(token, 16))
    return values


def emit_hybrid_top_byte_header(
    report: dict,
    handlers: list[str],
    output_path: Path,
    hot_bytes: list[int],
) -> None:
    stats = report["compression"]["top_byte"]
    block_index: list[int] = stats["block_index"]
    unique_blocks: list[list[str]] = stats["unique_block_data"]
    blocks = list(chunked(handlers, 256))
    valid_hot_bytes = [value for value in hot_bytes if 0 <= value < len(blocks)]
    hot_byte_set = set(valid_hot_bytes)
    index_type = c_integer_type(max(block_index) if block_index else 0)
    guard = sanitize_guard_token(output_path)
    table_name = output_path.stem

    hot_tables = []
    for value in valid_hot_bytes:
        hot_tables.append(
            f"static void (* const {table_name}_hot_{value:02x}[256])(void) =\n"
            "{\n"
            + format_c_rows(blocks[value], row_width=8, indent="    ")
            + "\n};"
        )
    hot_tables_blob = "\n\n".join(hot_tables)
    hot_cases = "\n".join(
        [
            f"        case 0x{value:02X}: {table_name}_hot_{value:02x}[lo](); return;"
            for value in valid_hot_bytes
        ]
    )
    hot_byte_literals = ", ".join(f"0x{value:02X}" for value in valid_hot_bytes)

    l1_values = [str(value) for value in block_index]
    subtables_rows = []
    for block in unique_blocks:
        subtables_rows.append(
            "    {\n"
            + format_c_rows(block, row_width=8, indent="      ")
            + "\n    }"
        )
    subtables_blob = ",\n".join(subtables_rows)

    content = f"""// Auto-generated by scripts/analyze_md_dispatch_table.py
// Source table: {report["table"]}
// Dense entries: {report["total_entries"]}
// Hybrid top-byte dispatch:
//   direct hot byte tables: {hot_byte_literals if valid_hot_bytes else "(none)"}
//   compressed fallback blocks: {stats["total_blocks"]} -> unique subtables: {stats["unique_blocks"]}
//
// Dispatch shape:
//   hi = opcode >> 8
//   lo = opcode & 0xff
//   if hi is hot: direct_hot_table[lo]()
//   else: subtables[l1[hi]][lo]()

#ifndef {guard}
#define {guard}

#include <stdint.h>

#define {table_name.upper()}_L1_COUNT {stats["total_blocks"]}
#define {table_name.upper()}_SUBTABLE_COUNT {stats["unique_blocks"]}
#define {table_name.upper()}_SUBTABLE_WIDTH 256
#define {table_name.upper()}_HOT_COUNT {len(valid_hot_bytes)}

static const uint8_t {table_name}_hot_bytes[{max(1, len(valid_hot_bytes))}] =
{{
{format_c_rows([f"0x{value:02X}" for value in valid_hot_bytes] if valid_hot_bytes else ["0x00"], row_width=16, indent="    ")}
}};

{hot_tables_blob if hot_tables_blob else "/* No hot top-byte tables selected. */"}

static const {index_type} {table_name}_l1[{stats["total_blocks"]}] =
{{
{format_c_rows(l1_values, row_width=16, indent="    ")}
}};

static void (* const {table_name}_subtables[{stats["unique_blocks"]}][256])(void) =
{{
{subtables_blob}
}};

static inline void {table_name}_dispatch(uint16_t opcode)
{{
    const unsigned hi = (unsigned)(opcode >> 8);
    const unsigned lo = (unsigned)(opcode & 0xff);
    switch (hi) {{
{hot_cases if hot_cases else "        default: break;"}
        default:
            break;
    }}
    if (hi >= {table_name.upper()}_L1_COUNT) {{
        m68k_op_illegal();
        return;
    }}
    {table_name}_subtables[{table_name}_l1[hi]][lo]();
}}

#endif /* {guard} */
"""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(content, encoding="utf-8")


def print_text_report(report: dict) -> None:
    print(f"Table: {report['table']}")
    print(
        "Entries: {total_entries} | illegal: {illegal_entries} ({illegal_pct:.2f}%) | "
        "unique handlers: {unique_handlers}".format(**report)
    )
    print()

    print("Top handlers:")
    for name, count in report["top_handlers"][:12]:
        pct = (count * 100.0) / report["total_entries"]
        print(f"  {name:<28} {count:>6}  {pct:6.2f}%")
    print()

    for key, title in (("top_byte", "Shared 256-entry subtables"), ("top_nibble", "Shared 4096-entry subtables")):
        stats = report["compression"][key]
        dense_kib = stats["dense_table_bytes"] / 1024.0
        indexed_kib = stats["estimated_total_bytes_indexed"] / 1024.0
        pointer_kib = stats["estimated_total_bytes_pointer_l1"] / 1024.0
        print(title + ":")
        print(
            f"  blocks={stats['total_blocks']} unique={stats['unique_blocks']} "
            f"reused={stats['reused_blocks']}"
        )
        print(
            f"  size dense={dense_kib:.1f} KiB | "
            f"indexed-L1={indexed_kib:.1f} KiB | pointer-L1={pointer_kib:.1f} KiB"
        )
        print("  most repeated layouts:")
        for layout in stats["top_layouts"][:5]:
            top_handlers = ", ".join(
                f"{name}:{count}" for name, count in layout["top_handlers"]
            )
            print(
                f"    idx={layout['layout_index']:>3} repeats={layout['count']:>3} "
                f"digest={layout['digest']} illegal={layout['all_illegal']} "
                f"blocks={layout['example_block_indices']} top=[{top_handlers}]"
            )
        print()

    print("Top-byte groups with highest illegal density:")
    top_byte_groups = sorted(
        report["groups"]["top_byte"],
        key=lambda item: (item["illegal"], item["unique_handlers"]),
        reverse=True,
    )[:12]
    for item in top_byte_groups:
        top_handlers = ", ".join(f"{name}:{count}" for name, count in item["top_handlers"][:3])
        print(
            f"  {item['group']}: illegal={item['illegal']:>3}/{item['entries']:<3} "
            f"({item['illegal_pct']:>6.2f}%) unique={item['unique_handlers']:>3} "
            f"top=[{top_handlers}]"
        )
    print()
    top_byte = report["compression"]["top_byte"]
    index_type = c_integer_type(max(top_byte["block_index"]) if top_byte["block_index"] else 0)
    print("Suggested compressed-dispatch basis:")
    print(
        f"  L1 entries={top_byte['total_blocks']} type={index_type} | "
        f"subtables={top_byte['unique_blocks']} x 256 handlers"
    )
    print(
        "  dispatch: subtables[l1[opcode >> 8]][opcode & 0xff]()"
    )


def main() -> int:
    args = parse_args()
    table_path = resolve_table_path(args)
    if not table_path.is_absolute():
        table_path = Path.cwd() / table_path
    handlers = load_handlers(table_path)
    report = build_report(table_path, handlers)

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_text_report(report)
    if args.emit_c_header is not None:
        output_path = args.emit_c_header
        if not output_path.is_absolute():
            output_path = Path.cwd() / output_path
        emit_top_byte_header(report, output_path)
        if not args.json:
            print()
            print(f"Wrote C proposal header: {output_path}")
    if args.emit_hybrid_c_header is not None:
        output_path = args.emit_hybrid_c_header
        if not output_path.is_absolute():
            output_path = Path.cwd() / output_path
        emit_hybrid_top_byte_header(
            report,
            handlers,
            output_path,
            parse_hot_bytes(args.hybrid_hot_bytes),
        )
        if not args.json:
            print()
            print(f"Wrote hybrid C proposal header: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
