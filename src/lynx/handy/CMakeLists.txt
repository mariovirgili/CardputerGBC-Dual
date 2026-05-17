file(GLOB HANDY_SRCS "*.cpp")

idf_component_register(
    SRCS ${HANDY_SRCS}
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fno-rtti
    -fno-exceptions
    -Wno-error=format
    -Wno-error=unused-value
    -Wno-format
)
