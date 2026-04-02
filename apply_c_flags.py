from SCons.Script import Import

Import("env")

# Compile Atari 2600/Stella and Atari 7800/ProSystem with exceptions enabled.
# Keep the rest of the project on -fno-exceptions to minimize side effects on
# legacy cores that do not require C++ exceptions.
atari_env = None


def get_atari_env(build_env):
    global atari_env
    if atari_env is None:
        atari_env = build_env.Clone()
        atari_env.ProcessUnFlags(["-fno-exceptions"])
        atari_env.ProcessFlags(["-fexceptions"])
    return atari_env


def route_atari_sources(build_env, node):
    source_path = node.srcnode().get_path().replace("\\", "/")
    if "/src/atari2600/" in source_path or "/src/atari7800/" in source_path or source_path.startswith("src/atari2600/") or source_path.startswith("src/atari7800/"):
        return get_atari_env(build_env).Object(node)
    return node


env.AddBuildMiddleware(route_atari_sources)

# Apply flags ONLY to C files (CFLAGS)
# This prevents the C++ compiler (CXX) from complaining about flags it doesn't understand.
# We suppress specific warnings for legacy emulator code (NES/NGP/SMS/GameBoy)
env.Append(CFLAGS=[
    "-Wno-incompatible-pointer-types",
    "-Wno-discarded-qualifiers",
    "-Wno-implicit-function-declaration",
    "-Wno-stringop-overflow",
    "-Wno-stringop-truncation",
    "-Wno-attributes",      # Fixes 'packed' attribute ignored on uint8 fields
    "-Wno-int-conversion",  # Fixes pointer generation from integer
    "-Wa,-W"                # Pass -W (suppress warnings) to the Assembler
])
