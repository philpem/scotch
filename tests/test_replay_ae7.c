#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/replay_ae7.h"

int main(void)
{
    static const char header[] =
        "ARMovie\nTest movie\nCopyright\nAuthor\n"
        "19 video format\n8 pixels\n4 pixels\n16 bits\n12.5 fps\n"
        "1 sound format\n11025 Hz\n2 channels\n8 bits\n"
        "25 frames per chunk\n01 last chunk\n64 even\n64 odd\n"
        "256 catalogue\n128 sprite\n32 sprite bytes\n-1 no keys\n";
    uint8_t file[512] = { 0U };
    static const char catalogue[] =
        "320,8;4\n"
        "384,16;2;3\n";
    ReplayAe7Movie movie;
    char error[128];
    ReplayStatus status;

    memcpy(file, header, sizeof(header) - 1U);
    memcpy(&file[256], catalogue, sizeof(catalogue) - 1U);
    status = replay_ae7_parse(file, sizeof(file), &movie,
                              error, sizeof(error));
    CHECK(status == REPLAY_OK);
    CHECK(strcmp(movie.title, "Test movie") == 0);
    CHECK(movie.video_codec == 19U);
    CHECK(movie.width == 8U);
    CHECK(movie.height == 4U);
    CHECK(movie.frames_per_second == 12.5);
    CHECK(movie.last_chunk == 1U);
    CHECK(movie.chunk_count == 2U);
    CHECK(movie.chunks[0].file_offset == 320U);
    CHECK(movie.chunks[0].video_bytes == 8U);
    CHECK(movie.chunks[0].sound_bytes == 4U);
    CHECK(movie.chunks[0].sound_tracks == 1U);
    CHECK(movie.chunks[1].sound_bytes == 5U);
    CHECK(movie.chunks[1].sound_tracks == 2U);
    CHECK(movie.key_frame_offset == -1);
    replay_ae7_movie_destroy(&movie);

    file[256] = 'x';
    status = replay_ae7_parse(file, sizeof(file), &movie,
                              error, sizeof(error));
    CHECK(status == REPLAY_MALFORMED_STREAM);

    memcpy(&file[256], catalogue, sizeof(catalogue) - 1U);
    /* Header line 15 is the last chunk index. A huge value must be rejected
       before allocation because the remaining file cannot hold that table. */
    {
        char *last_chunk = strstr((char *)file, "01 last chunk");
        CHECK(last_chunk != NULL);
        memcpy(last_chunk, "99 last chunk", sizeof("99 last chunk") - 1U);
    }
    status = replay_ae7_parse(file, sizeof(file), &movie,
                              error, sizeof(error));
    CHECK(status == REPLAY_TRUNCATED_INPUT);

    /* Real-world tolerances (see docs/spec/ae7-armovie-container.md §2/§3):
       - a blank numeric sound field (e.g. !Morpheus writes an empty bits-per-
         sample line when silent) counts as zero;
       - the sound rate may be a sample period in microseconds ("72 µs samples")
         and must be reported back in Hz;
       - a catalogue line may end with a bare ";" (no count) for a silent chunk. */
    {
        static const char tol_header[] =
            "ARMovie\nTest\nCopyright\nAuthor\n"
            "1 video format\n8 pixels\n4 pixels\n16 bits per pixel\n12.5 fps\n"
            "0 no sound\n72 \xB5s samples\n0\n\n"   /* blank precision line */
            "25 frames per chunk\n01 last chunk\n64 even\n64 odd\n"
            "256 catalogue\n128 sprite\n32 sprite bytes\n-1 no keys\n";
        static const char tol_catalogue[] =
            "320,8;\n"        /* bare trailing ';' -- silent chunk */
            "384,16;\n";
        uint8_t tol[512] = { 0U };

        memcpy(tol, tol_header, sizeof(tol_header) - 1U);
        memcpy(&tol[256], tol_catalogue, sizeof(tol_catalogue) - 1U);
        status = replay_ae7_parse(tol, sizeof(tol), &movie,
                                  error, sizeof(error));
        CHECK(status == REPLAY_OK);
        CHECK(movie.sound_precision == 0U);          /* blank line -> 0 */
        CHECK(movie.sound_rate == 13889U);           /* 1e6 / 72 us, rounded */
        CHECK(movie.chunk_count == 2U);
        CHECK(movie.chunks[0].video_bytes == 8U);
        CHECK(movie.chunks[0].sound_bytes == 0U);    /* bare ';' -> no sound */
        CHECK(movie.chunks[0].sound_tracks == 0U);
        CHECK(movie.chunks[1].video_bytes == 16U);
        CHECK(movie.chunks[1].sound_bytes == 0U);
        replay_ae7_movie_destroy(&movie);
    }
    return EXIT_SUCCESS;
}
