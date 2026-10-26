#include <doctest/doctest.h>

#include "hardware/screen/sharp_memory_protocol.h"

TEST_CASE("Sharp Memory LCD protocol reverses bytes for MSB-first SPI") {
    CHECK(sharp_memory_reverse_byte(0x01) == 0x80);
    CHECK(sharp_memory_reverse_byte(0x02) == 0x40);
    CHECK(sharp_memory_reverse_byte(0x04) == 0x20);
    CHECK(sharp_memory_reverse_byte(0x96) == 0x69);
}

TEST_CASE("Sharp Memory LCD protocol encodes a line write") {
    const uint8_t frame[] = {0x01};
    uint8_t output[5] = {};

    CHECK(sharp_memory_encoded_rows_size(8, 1) == sizeof(output));
    CHECK(sharp_memory_encode_rows(output, sizeof(output), frame, 8, 0, 1, false) == sizeof(output));
    CHECK(output[0] == 0x80); // write command
    CHECK(output[1] == 0x80); // one-based line address 1
    CHECK(output[2] == 0x80); // first pixel is white
    CHECK(output[3] == 0x00); // line trailer
    CHECK(output[4] == 0x00); // transaction trailer
}

TEST_CASE("Sharp Memory LCD protocol includes VCOM and absolute line address") {
    const uint8_t frame[] = {0x00, 0x81};
    uint8_t output[5] = {};

    CHECK(sharp_memory_encode_rows(output, sizeof(output), frame, 8, 1, 1, true) == sizeof(output));
    CHECK(output[0] == 0xc0); // write | VCOM
    CHECK(output[1] == 0x40); // one-based line address 2
    CHECK(output[2] == 0x81);
}

TEST_CASE("A 60-line panel chunk fits the default spidev transfer buffer") {
    uint8_t frame[400 * 240 / 8] = {};
    uint8_t output[3122] = {};
    frame[180 * 50] = 0x01;

    CHECK(sharp_memory_encoded_rows_size(400, 60) == sizeof(output));
    CHECK(sharp_memory_encode_rows(output, sizeof(output), frame, 400, 180, 60, false) ==
          sizeof(output));
    CHECK(output[0] == sharp_memory_reverse_byte(SHARP_MEMORY_WRITE_COMMAND));
    CHECK(output[1] == sharp_memory_reverse_byte(181));
    CHECK(output[2] == 0x80);
    CHECK(output[1 + 59 * 52] == sharp_memory_reverse_byte(240));
    CHECK(output[sizeof(output) - 1] == 0x00);
}

TEST_CASE("Sharp Memory LCD protocol rejects invalid buffers and rows") {
    const uint8_t frame[] = {0x00};
    uint8_t output[5] = {};

    CHECK(sharp_memory_encoded_rows_size(7, 1) == 0);
    CHECK(sharp_memory_encoded_rows_size(8, 0) == 0);
    CHECK(sharp_memory_encode_rows(output, 4, frame, 8, 0, 1, false) == 0);
    CHECK(sharp_memory_encode_rows(output, sizeof(output), frame, 8, 255, 1, false) == 0);
}

TEST_CASE("Sharp Memory LCD protocol encodes clear and VCOM-only commands") {
    uint8_t output[2] = {};

    CHECK(sharp_memory_encode_command(output, sizeof(output), SHARP_MEMORY_CLEAR_COMMAND, true) == 2);
    CHECK(output[0] == 0x60);
    CHECK(output[1] == 0x00);

    CHECK(sharp_memory_encode_command(output, sizeof(output), 0, false) == 2);
    CHECK(output[0] == 0x00);
    CHECK(output[1] == 0x00);
}
