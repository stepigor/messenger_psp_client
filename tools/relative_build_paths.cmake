# Use CMake's spelling of the paths, including on hosts with directory aliases.
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=./source"
                    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build")
