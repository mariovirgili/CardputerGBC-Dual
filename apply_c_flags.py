from SCons.Script import Import

Import("env")

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