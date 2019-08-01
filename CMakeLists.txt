# Previous to 3.15 cmake would override the ASM_NASM_COMPILE_OBJECT
cmake_minimum_required(VERSION 3.15)

set(CMAKE_ASM_NASM_LINK_EXECUTABLE "ld <CMAKE_ASM_NASM_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_ASM_NASM_FLAGS_DEBUG "-g -Fdwarf")
set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> <FLAGS> -o <OBJECT> <SOURCE>")
add_compile_options(
    "$<$<COMPILE_LANGUAGE:ASM_NASM>:-f $<IF:$<BOOL:$<TARGET_PROPERTY:NASM_OBJ_FORMAT>>, \
    $<TARGET_PROPERTY:NASM_OBJ_FORMAT>, ${CMAKE_ASM_NASM_OBJECT_FORMAT}>>"
)

project(vgasm C ASM_NASM)

add_executable(main
    src/main.c src/queue.c src/fiber.c
    src/log.c src/scheduler.c
    src/asm/x86_64_sysv_elf_nasm.asm
)
target_include_directories(main PRIVATE "include")
target_link_libraries(main pthread)
target_compile_options(main PRIVATE -Wall -Wpedantic)
set_target_properties(main PROPERTIES NASM_OBJ_FORMAT elf64)
