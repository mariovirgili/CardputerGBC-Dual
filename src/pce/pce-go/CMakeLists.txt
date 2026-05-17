file(GLOB PCE_GO_SRCS "*.c")

idf_component_register(
    SRCS ${PCE_GO_SRCS}
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-sequence-point -Wno-unused -Wno-error=format)
