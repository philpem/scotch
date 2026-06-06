cmake_minimum_required(VERSION 3.20)
project(replay_tooling VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(replay STATIC
    src/replay_status.c
    src/replay_buffer.c
    src/replay_bitstream.c
    src/mb_huffman.c
    src/mb_codec.c
    src/codec_movingblocks.c
    src/codec_movingblockshq.c
    src/codec_supermovingblocks.c
    src/codec_movingblocksbeta.c
)

target_include_directories(replay PUBLIC include)

if(MSVC)
    target_compile_options(replay PRIVATE /W4 /WX)
else()
    target_compile_options(replay PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(replay-verify tools/replay_verify.c)
target_link_libraries(replay-verify PRIVATE replay)

enable_testing()

function(add_replay_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE replay)
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_replay_test(test_replay_buffer tests/test_replay_buffer.c)
add_replay_test(test_replay_bitstream tests/test_replay_bitstream.c)
add_replay_test(test_mb_huffman tests/test_mb_huffman.c)
add_replay_test(test_mb_codecs tests/test_mb_codecs.c)
add_replay_test(test_codec_supermovingblocks
    tests/test_codec_supermovingblocks.c)
