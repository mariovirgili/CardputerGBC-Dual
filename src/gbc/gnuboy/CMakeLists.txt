file(GLOB GNUBOY_SRCS "*.c")

idf_component_register(
    SRCS ${GNUBOY_SRCS}
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
