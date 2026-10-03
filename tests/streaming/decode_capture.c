/* Offline verification of captured elementary streams; this is not a renderer. */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>

#define INPUT_LIMIT (100U * 1024U * 1024U)
#define PACKET_LIMIT (8U * 1024U * 1024U)
#define FRAME_LIMIT 100000U
#define PIXEL_LIMIT (4096 * 2160)

typedef struct {
    uint64_t input_bytes;
    uint64_t csv_bytes;
    unsigned csv_frames;
    unsigned parsed_packets;
    unsigned decoded_frames;
    unsigned corrupt_frames;
    unsigned dimension_changes;
    unsigned decoder_log_errors;
    unsigned api_errors;
    int width;
    int height;
    int first_error;
    bool decoder_available;
    bool parser_available;
    bool csv_checked;
    bool csv_valid;
    const char* packetization;
    const char* status;
} result_t;

static result_t result;
static uint32_t csv_packet_sizes[FRAME_LIMIT];

static void decoder_log(void* context, int level, const char* format, va_list args) {
    (void)context;
    (void)format;
    (void)args;
    if (level <= AV_LOG_ERROR && result.decoder_log_errors != UINT32_MAX) {
        result.decoder_log_errors++;
    }
}

static void record_error(int error) {
    if (!result.first_error) result.first_error = error;
    result.api_errors++;
}

static bool csv_number(char** cursor, uint64_t* value, char separator) {
    if (**cursor < '0' || **cursor > '9') return false;
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = strtoull(*cursor, &end, 10);
    if (errno == ERANGE || end == *cursor || *end != separator) return false;
    *value = (uint64_t)parsed;
    *cursor = end + 1;
    return true;
}

static bool check_csv(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    char line[256];
    bool valid = fgets(line, sizeof(line), file) != NULL &&
        (strcmp(line, "frame,type,bytes,first_receive_us,submit_us\n") == 0 ||
         strcmp(line, "frame,type,bytes,first_receive_us,submit_us\r\n") == 0);
    uint64_t previous_frame = 0;
    while (valid && fgets(line, sizeof(line), file)) {
        uint64_t frame, type, bytes, receive, submit;
        char* cursor = line;
        const size_t length = strlen(line);
        if (!length || line[length - 1] != '\n') { valid = false; break; }
        line[length - 1] = '\0';
        if (length > 1 && line[length - 2] == '\r') line[length - 2] = '\0';
        if (!csv_number(&cursor, &frame, ',') || !csv_number(&cursor, &type, ',') ||
            !csv_number(&cursor, &bytes, ',') || !csv_number(&cursor, &receive, ',') ||
            !csv_number(&cursor, &submit, '\0') ||
            !frame || frame <= previous_frame || type > 1 || !bytes || bytes > PACKET_LIMIT ||
            result.csv_bytes + bytes > INPUT_LIMIT || submit < receive ||
            result.csv_frames == FRAME_LIMIT) {
            valid = false;
            break;
        }
        previous_frame = frame;
        result.csv_bytes += bytes;
        csv_packet_sizes[result.csv_frames] = (uint32_t)bytes;
        result.csv_frames++;
    }
    if (ferror(file)) valid = false;
    fclose(file);
    return valid && result.csv_frames > 0 && result.csv_bytes == result.input_bytes;
}

static bool receive_frames(AVCodecContext* codec, AVFrame* frame) {
    for (;;) {
        const int error = avcodec_receive_frame(codec, frame);
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return true;
        if (error < 0) {
            record_error(error);
            return false;
        }
        if (result.decoded_frames == FRAME_LIMIT || frame->width <= 0 || frame->height <= 0 ||
            (int64_t)frame->width * frame->height > PIXEL_LIMIT) {
            record_error(AVERROR(EINVAL));
            av_frame_unref(frame);
            return false;
        }
        if (result.decoded_frames &&
            (frame->width != result.width || frame->height != result.height)) {
            result.dimension_changes++;
        }
        result.width = frame->width;
        result.height = frame->height;
        result.decoded_frames++;
        if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) result.corrupt_frames++;
        av_frame_unref(frame);
    }
}

static bool send_packet(AVCodecContext* codec, AVPacket* packet, AVFrame* frame,
                        const uint8_t* bytes, int size) {
    if (size <= 0 || (unsigned)size > PACKET_LIMIT || result.parsed_packets == FRAME_LIMIT) {
        record_error(AVERROR(EINVAL));
        return false;
    }
    av_packet_unref(packet);
    int error = av_new_packet(packet, size);
    if (error < 0) {
        record_error(error);
        return false;
    }
    memcpy(packet->data, bytes, (size_t)size);
    result.parsed_packets++;
    error = avcodec_send_packet(codec, packet);
    if (error < 0) {
        record_error(error);
        return false;
    }
    return receive_frames(codec, frame);
}

static bool decode(AVCodecContext* codec, AVCodecParserContext* parser,
                   uint8_t* bytes, size_t size) {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool valid = packet && frame;
    uint8_t *parser_input = parser ? av_malloc(65536 + AV_INPUT_BUFFER_PADDING_SIZE) : NULL;
    if (parser && !parser_input) {
      record_error(AVERROR(ENOMEM));
      valid = false;
    }
    size_t offset = 0;
    unsigned zero_progress = 0;
    if (!parser) {
        /* DECODE_UNIT lengths preserve the actual client frame boundaries. */
        for (unsigned index = 0; valid && index < result.csv_frames; index++) {
            valid = send_packet(codec, packet, frame, bytes + offset, (int)csv_packet_sizes[index]);
            offset += csv_packet_sizes[index];
        }
    }
    while (valid && offset < size) {
        uint8_t* output = NULL;
        int output_size = 0;
        const int chunk = (int)((size - offset) > 65536 ? 65536 : size - offset);
        /* Every parser input needs its own zero tail, including interior chunks. */
        memcpy(parser_input, bytes + offset, (size_t) chunk);
        memset(parser_input + chunk, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        const int used = av_parser_parse2(parser, codec, &output, &output_size,
          parser_input, chunk, AV_NOPTS_VALUE, AV_NOPTS_VALUE, (int64_t) offset);
        if (used < 0 || used > chunk || (!used && !output_size) || (!used && ++zero_progress > 2)) {
            record_error(used < 0 ? used : AVERROR_INVALIDDATA);
            valid = false;
            break;
        }
        if (used) zero_progress = 0;
        offset += (size_t)used;
        if (output_size) valid = send_packet(codec, packet, frame, output, output_size);
    }
    if (valid && parser) {
        uint8_t* output = NULL;
        int output_size = 0;
        const int used = av_parser_parse2(parser, codec, &output, &output_size,
            NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, (int64_t)offset);
        if (used < 0) {
            record_error(used);
            valid = false;
        } else if (output_size) {
            valid = send_packet(codec, packet, frame, output, output_size);
        }
    }
    if (valid) {
        const int error = avcodec_send_packet(codec, NULL);
        if (error < 0) {
            record_error(error);
            valid = false;
        } else {
            valid = receive_frames(codec, frame);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_free(parser_input);
    return valid;
}

static void print_json(void) {
    printf("{\"status\":\"%s\",\"avcodec_version\":%u,\"input_bytes\":\"%" PRIu64
        "\",\"decoder_available\":%s,\"parser_available\":%s,\"parsed_packets\":%u,"
        "\"decoded_frames\":%u,\"width\":%d,\"height\":%d,\"dimension_changes\":%u,"
        "\"corrupt_frames\":%u,\"decoder_log_errors\":%u,\"api_errors\":%u,"
        "\"first_error\":%d,\"csv_checked\":%s,\"csv_valid\":%s,\"csv_frames\":%u,"
        "\"csv_bytes\":\"%" PRIu64 "\",\"packetization\":\"%s\",\"offline_only\":true}\n",
        result.status, avcodec_version(), result.input_bytes,
        result.decoder_available ? "true" : "false", result.parser_available ? "true" : "false",
        result.parsed_packets, result.decoded_frames, result.width, result.height,
        result.dimension_changes, result.corrupt_frames, result.decoder_log_errors, result.api_errors,
        result.first_error, result.csv_checked ? "true" : "false", result.csv_valid ? "true" : "false",
        result.csv_frames, result.csv_bytes, result.packetization);
}

int main(int argc, char** argv) {
    result.status = "invalid_input";
    result.packetization = "none";
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s capture.h264 [capture.frames.csv]\n", argv[0]);
        print_json();
        return 2;
    }
    FILE* file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        print_json();
        return 2;
    }
    /* Windows long is sufficient for the enforced 100 MiB bound. */
    const long measured = ftell(file);
    if (measured <= 0 || (unsigned long)measured > INPUT_LIMIT) {
        fclose(file);
        print_json();
        return 2;
    }
    const size_t size = (size_t)measured;
    if (fseek(file, 0, SEEK_SET)) { fclose(file); print_json(); return 2; }
    result.input_bytes = size;
    uint8_t* bytes = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!bytes || fread(bytes, 1, size, file) != size || fgetc(file) != EOF || ferror(file)) {
        av_free(bytes);
        fclose(file);
        print_json();
        return 2;
    }
    fclose(file);
    if (argc == 3) {
        result.csv_checked = true;
        result.csv_valid = check_csv(argv[2]);
    }
    av_log_set_callback(decoder_log);
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    result.decoder_available = decoder != NULL;
    result.parser_available = parser != NULL;
    int exit_code = 3;
    if (result.csv_checked && !result.csv_valid) {
        result.status = "capture_csv_mismatch";
        exit_code = 4;
    } else if (!decoder || (!parser && !result.csv_valid)) {
        result.status = "h264_decoder_or_parser_unavailable";
    } else {
        result.packetization = parser ? "h264_parser" : "capture_csv";
        AVCodecContext* codec = avcodec_alloc_context3(decoder);
        if (codec) {
            codec->thread_count = 1;
            codec->max_pixels = PIXEL_LIMIT;
            codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
            const int error = avcodec_open2(codec, decoder, NULL);
            if (error < 0) record_error(error);
            const bool valid = error >= 0 && decode(codec, parser, bytes, size);
            const bool csv_matches = !result.csv_checked ||
                (result.csv_valid && result.csv_frames == result.decoded_frames &&
                 result.csv_frames == result.parsed_packets);
            if (valid && result.decoded_frames && !result.corrupt_frames &&
                !result.decoder_log_errors && !result.api_errors && csv_matches) {
                result.status = "decoded";
                exit_code = 0;
            } else {
                result.status = !valid || !result.decoded_frames || result.corrupt_frames ||
                    result.decoder_log_errors || result.api_errors ? "decode_failed" : "capture_csv_mismatch";
                exit_code = 4;
            }
        } else {
            record_error(AVERROR(ENOMEM));
            result.status = "decoder_allocation_failed";
        }
        avcodec_free_context(&codec);
    }
    av_parser_close(parser);
    av_free(bytes);
    print_json();
    return exit_code;
}
