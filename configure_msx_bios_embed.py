from pathlib import Path

from SCons.Script import Import

Import("env")


def option_enabled(value):
    return str(value).strip().lower() in ("1", "true", "yes", "on")


project_dir = Path(env.subst("$PROJECT_DIR"))
section = f"env:{env['PIOENV']}"


def ensure_embed_file(config, embed_rel_path):
    existing_files = [
        line.strip().replace("\\", "/")
        for line in str(config.get(section, "board_build.embed_files", "")).splitlines()
        if line.strip()
    ]
    embed_file = embed_rel_path.as_posix()
    if embed_file not in existing_files:
        existing_files.append(embed_file)
        config.set(section, "board_build.embed_files", existing_files)
    return embed_file


rom_options = (
    {
        "option": "custom_msx2_embed",
        "path": Path("bios/private/MSX2.ROM"),
        "define": ("MSX_EMBED_OFFICIAL_MSX2", 1),
        "label": "official MSX2 ROM",
    },
    {
        "option": "custom_msx2ext_embed",
        "path": Path("bios/private/MSX2EXT.ROM"),
        "define": ("MSX_EMBED_OFFICIAL_MSX2EXT", 1),
        "label": "official MSX2EXT ROM",
    },
)

config = env.GetProjectConfig()

for rom_option in rom_options:
    if not option_enabled(env.GetProjectOption(rom_option["option"], "no")):
        continue

    rom_path = project_dir / rom_option["path"]
    if not rom_path.is_file():
        print(
            f'Error: {rom_option["option"]} is enabled but "{rom_path}" was not found.'
        )
        print(
            f'Place your {rom_option["label"]} at {rom_option["path"].as_posix()} and rebuild.'
        )
        env.Exit(1)

    embed_file = ensure_embed_file(config, rom_option["path"])
    env.AppendUnique(CPPDEFINES=[rom_option["define"]])
    print(f"[msx-bios] embedding {rom_option['label']} from {embed_file}")
