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
    src/replay_ae7.c
    src/replay_type2.c
    src/replay_type23.c
    src/replay_ae7_write.c
    src/replay_movie.c
    src/replay_sound.c
    src/mb_encode.c
    src/mb_color.c
    src/mb_huffman.c
    src/mb_codec.c
    src/mb_frame_verify.c
    src/mb_motion.c
    src/mb_metrics.c
    src/mb_quality.c
    src/mb_rate_control.c
    src/mb_repeat.c
    src/codec_movingblocks.c
    src/codec_movingblockshq.c
    src/codec_supermovingblocks.c
    src/codec_movingblocksbeta.c
)

target_include_directories(replay PUBLIC include)

if(NOT MSVC)
    target_link_libraries(replay PUBLIC m)
endif()

if(MSVC)
    target_compile_options(replay PRIVATE /W4 /WX)
else()
    target_compile_options(replay PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(replay-verify tools/replay_verify.c)
target_link_libraries(replay-verify PRIVATE replay)

add_executable(replay-encode tools/replay_encode.c)
target_link_libraries(replay-encode PRIVATE replay)

add_executable(replay-inspect tools/replay_inspect.c)
target_link_libraries(replay-inspect PRIVATE replay)

add_executable(replay-extract tools/replay_extract.c)
target_link_libraries(replay-extract PRIVATE replay)

add_executable(replay-join tools/replay_join.c)
target_link_libraries(replay-join PRIVATE replay)

if(NOT MSVC)
    target_compile_options(replay-verify PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_compile_options(replay-encode PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_compile_options(replay-inspect PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_compile_options(replay-extract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_compile_options(replay-join PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

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
add_replay_test(test_replay_ae7 tests/test_replay_ae7.c)
add_replay_test(test_replay_ae7_write tests/test_replay_ae7_write.c)
add_replay_test(test_replay_sound tests/test_replay_sound.c)
add_replay_test(test_replay_type2 tests/test_replay_type2.c)
add_replay_test(test_replay_type23 tests/test_replay_type23.c)
add_replay_test(test_mb_huffman tests/test_mb_huffman.c)
add_replay_test(test_mb_codecs tests/test_mb_codecs.c)
add_replay_test(test_mb_motion tests/test_mb_motion.c)
add_replay_test(test_mb_encode tests/test_mb_encode.c)
add_replay_test(test_mb_metrics tests/test_mb_metrics.c)
add_replay_test(test_mb_quality tests/test_mb_quality.c)
add_replay_test(test_mb_rate_control tests/test_mb_rate_control.c)
add_replay_test(test_mb_repeat tests/test_mb_repeat.c)
add_replay_test(test_mb_color tests/test_mb_color.c)
add_replay_test(test_mb_color_complib tests/test_mb_color_complib.c)
add_replay_test(test_codec_supermovingblocks
    tests/test_codec_supermovingblocks.c)
add_replay_test(test_codec_movingblockshq
    tests/test_codec_movingblockshq.c)
add_replay_test(test_codec_movingblocks
    tests/test_codec_movingblocks.c)
add_replay_test(test_codec_movingblocksbeta
    tests/test_codec_movingblocksbeta.c)
add_replay_test(test_supermovingblocks_frame
    tests/test_supermovingblocks_frame.c)
add_replay_test(test_supermovingblocks_encoder
    tests/test_supermovingblocks_encoder.c)
add_replay_test(test_supermovingblocks_stationary
    tests/test_supermovingblocks_stationary.c)
add_replay_test(test_supermovingblocks_temporal
    tests/test_supermovingblocks_temporal.c)
add_replay_test(test_supermovingblocks_spatial
    tests/test_supermovingblocks_spatial.c)
add_replay_test(test_supermovingblocks_split
    tests/test_supermovingblocks_split.c)
add_replay_test(test_supermovingblocks_split_motion
    tests/test_supermovingblocks_split_motion.c)
add_replay_test(test_supermovingblocks_trace
    tests/test_supermovingblocks_trace.c)

add_executable(make_decomp19_crosscheck_fixtures
    tests/make_decomp19_crosscheck_fixtures.c)
target_link_libraries(make_decomp19_crosscheck_fixtures PRIVATE replay)
if(NOT MSVC)
    target_compile_options(make_decomp19_crosscheck_fixtures PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(make_decomp17_crosscheck_fixtures
    tests/make_decomp17_crosscheck_fixtures.c)
target_link_libraries(make_decomp17_crosscheck_fixtures PRIVATE replay)
if(NOT MSVC)
    target_compile_options(make_decomp17_crosscheck_fixtures PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(make_decomp7_crosscheck_fixtures
    tests/make_decomp7_crosscheck_fixtures.c)
target_link_libraries(make_decomp7_crosscheck_fixtures PRIVATE replay)
if(NOT MSVC)
    target_compile_options(make_decomp7_crosscheck_fixtures PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(make_fullmovie_crosscheck_fixtures
    tests/make_fullmovie_crosscheck_fixtures.c)
target_link_libraries(make_fullmovie_crosscheck_fixtures PRIVATE replay)
if(NOT MSVC)
    target_compile_options(make_fullmovie_crosscheck_fixtures PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(make_decomp20_crosscheck_fixtures
    tests/make_decomp20_crosscheck_fixtures.c)
target_link_libraries(make_decomp20_crosscheck_fixtures PRIVATE replay)
if(NOT MSVC)
    target_compile_options(make_decomp20_crosscheck_fixtures PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_test(NAME test_replay_verify_cli
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_verify_cli.sh
        $<TARGET_FILE:replay-verify>)
add_test(NAME test_replay_encode_cli
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_encode_cli.sh
        $<TARGET_FILE:replay-encode>
        $<TARGET_FILE:replay-verify>)
add_test(NAME test_format19_corpus
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_format19_corpus.sh
        $<TARGET_FILE:replay-verify>)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME test_direct_container
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_direct_container.sh
            $<TARGET_FILE:replay-encode>
            $<TARGET_FILE:replay-join>
            $<TARGET_FILE:replay-inspect>)
endif()
find_program(FFMPEG_EXECUTABLE ffmpeg)
if(FFMPEG_EXECUTABLE)
    add_test(NAME test_ffmpeg_pipe
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffmpeg_pipe.sh
            ${FFMPEG_EXECUTABLE}
            $<TARGET_FILE:replay-encode>
            $<TARGET_FILE:replay-verify>)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME test_mb19_quality_sweep
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mb19_quality_sweep.sh
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/mb19_quality_sweep.py
            $<TARGET_FILE:replay-encode>
            $<TARGET_FILE:replay-verify>)
    add_test(NAME test_decomp19_unicorn
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp19_unicorn.sh
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py)
    set_tests_properties(test_decomp19_unicorn PROPERTIES SKIP_RETURN_CODE 77)

    set(DECOMP19_COMPILED
        "${CMAKE_CURRENT_SOURCE_DIR}/../!ARMovie_compiled/Decomp19/Decompress,ffd"
        CACHE FILEPATH "Compiled Acorn format-19 Decompress file")
    if(EXISTS "${DECOMP19_COMPILED}")
        add_test(NAME test_decomp19_compiled
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp19_compiled.sh
                $<TARGET_FILE:make_decomp19_crosscheck_fixtures>
                $<TARGET_FILE:replay-verify>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP19_COMPILED})
        add_test(NAME test_fullmovie_decomp19
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fullmovie_crosscheck.sh
                $<TARGET_FILE:make_fullmovie_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP19_COMPILED} 19 6y5uv)
    endif()
    set(DECOMP23_COMPILED
        "${CMAKE_CURRENT_SOURCE_DIR}/../!ARMovie_compiled/Decomp23/Decompress,ffd"
        CACHE FILEPATH "Compiled Acorn format-23 Decompress file")
    if(EXISTS "${DECOMP23_COMPILED}")
        add_test(NAME test_decomp23_compiled
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp23_compiled.sh
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP23_COMPILED})
    endif()
    set(DECOMP17_COMPILED
        "${CMAKE_CURRENT_SOURCE_DIR}/../!ARMovie_compiled/Decomp17/Decompress,ffd"
        CACHE FILEPATH "Compiled Acorn format-17 Decompress file")
    if(EXISTS "${DECOMP17_COMPILED}")
        add_test(NAME test_decomp17_compiled
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp17_compiled.sh
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP17_COMPILED})
        add_test(NAME test_decomp17_encoded
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp17_encoded.sh
                $<TARGET_FILE:make_decomp17_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP17_COMPILED})
        add_test(NAME test_fullmovie_decomp17
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fullmovie_crosscheck.sh
                $<TARGET_FILE:make_fullmovie_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP17_COMPILED} 17 yuv555)
    endif()
    set(DECOMP7_COMPILED
        "${CMAKE_CURRENT_SOURCE_DIR}/../!ARMovie_compiled/Decomp7/Decompress,ffd"
        CACHE FILEPATH "Compiled Acorn format-7 Decompress file")
    if(EXISTS "${DECOMP7_COMPILED}")
        add_test(NAME test_decomp7_compiled
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp7_compiled.sh
                $<TARGET_FILE:make_decomp7_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP7_COMPILED})
        add_test(NAME test_fullmovie_decomp7
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fullmovie_crosscheck.sh
                $<TARGET_FILE:make_fullmovie_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP7_COMPILED} 7 yuv555)
    endif()
    set(DECOMP20_COMPILED
        "${CMAKE_CURRENT_SOURCE_DIR}/../!ARMovie_compiled/Decomp20/Decompress,ffd"
        CACHE FILEPATH "Compiled Acorn format-20 Decompress file")
    if(EXISTS "${DECOMP20_COMPILED}")
        add_test(NAME test_decomp20_compiled
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp20_compiled.sh
                $<TARGET_FILE:make_decomp20_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP20_COMPILED} old)
        add_test(NAME test_fullmovie_decomp20
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fullmovie_crosscheck.sh
                $<TARGET_FILE:make_fullmovie_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP20_COMPILED} 20 6y6uv)
    endif()
    set(DECOMP20NEW_COMPILED
        "${CMAKE_CURRENT_SOURCE_DIR}/../!ARMovie_compiled/Decomp20new/Decompress,ffd"
        CACHE FILEPATH "Compiled Acorn format-20 v0.05 (delta chroma) Decompress")
    if(EXISTS "${DECOMP20NEW_COMPILED}")
        add_test(NAME test_decomp20new_compiled
            COMMAND sh
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decomp20_compiled.sh
                $<TARGET_FILE:make_decomp20_crosscheck_fixtures>
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp19_unicorn.py
                ${DECOMP20NEW_COMPILED} new)
    endif()
endif()
