/*
 * jxlw_cli - convert a JPEG / JPEG XL picture into the other format.
 *
 *   jxlw_cli [options] [input.jpg|input.jxl]
 *
 * The picture is decoded with the jxlw library and written again in the other
 * format, next to the input file and with the same base name (".jpg" and
 * ".jxl" are swapped).
 *
 * The api used to read and to write the picture is picked at random:
 *   - jxlw_reader_new_from_file()   or   jxlw_reader_new_from_memory()
 *   - jxlw_writer_unref_to_file()   or   jxlw_writer_unref_to_bytes()
 * so that all the code paths of the library are exercised.
 *
 * When no file is given, a file dialog is shown (tinyfiledialogs).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tinyfiledialogs.h"
#include "jxlw.h"

#define APP_PATH_MAX 1024

typedef struct _app_options {
    char input[APP_PATH_MAX];        // the input file
    char output[APP_PATH_MAX];       // the transcoded file
    bool have_output;                // the output path was given on the command line
    jxlw_codec_type_e target;        // the format of the result
    int quality;                     // jxlw_writer_quality_e
    uint32_t seed;                   // the seed of the random generator
    uint32_t count;                  // how many times the conversion is done
    bool dialog;                     // show the file dialog when there is no input
    bool verify;                     // decode the result again to check it
} app_options_t;

typedef struct _app {
    app_options_t opt;
    jxlw_reader_t* reader;
    jxlw_writer_t* writer;
} app_t;

#pragma region misc
static bool app_str_ieq(const char* a, const char* b) {
    while (*a && *b) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;
}

// 复制一个字符串，太长的时候截断
static void app_strcpy(char* dst, size_t size, const char* src) {
    if (!size) return;
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char* app_format_name(jxlw_color_format_e format) {
    switch (format) {
    case JXLW_COLOR_FORMAT_GRAY: return "gray";
    case JXLW_COLOR_FORMAT_GRAYA: return "gray+alpha";
    case JXLW_COLOR_FORMAT_RGB: return "rgb";
    case JXLW_COLOR_FORMAT_RGBA: return "rgb+alpha";
    default: break;
    }
    return "unknown";
}

static const char* app_type_name(jxlw_codec_type_e type) {
    switch (type) {
    case JXLW_CODEC_TYPE_JXL: return "jxl";
    case JXLW_CODEC_TYPE_JPEG: return "jpeg";
    default: break;
    }
    return "unknown";
}

/**
 * Build the path of the result.
 *
 * The extension of the input is replaced, so that the result stays in the
 * directory of the input.
 */
static bool app_make_output(const char* path, bool to_jxl, char* output, size_t size) {
    const char* ext = to_jxl ? ".jxl" : ".jpg";
    const char* base = path + strlen(path);
    // 去掉扩展名，目录里的点要保留
    while (base > path) {
        const char c = base[-1];
        if ('.' == c) {
            base--;
            break;
        }
        if ('/' == c || '\\' == c) break;
        base--;
    }
    const size_t len = (size_t)(base - path);
    if (len + strlen(ext) + 1 > size) return false;
    memcpy(output, path, len);
    strcpy(output + len, ext);
    return true;
}
#pragma endregion // misc

#pragma region random
/**
 * A tiny xorshift generator.
 *
 * The seed can be given on the command line, so that a failing run can be
 * reproduced.
 */
static uint32_t g_rand_state = 1;

static void app_srand(uint32_t seed) {
    g_rand_state = seed ? seed : 0x9E3779B9u;
}

static uint32_t app_rand() {
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

// 随机地选一个：true 或者 false
static bool app_rand_bool() {
    return 0 != (app_rand() & 1);
}
#pragma endregion // random

#pragma region file
/**
 * Read the whole file into a new buffer.
 *
 * The buffer belongs to the caller, it is released with free().
 */
static uint8_t* app_read_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (0 != fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NULL;
    }
    const long len = ftell(f);
    if (0 >= len) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t* buff = (uint8_t*)malloc((size_t)len);
    if (!buff) {
        fclose(f);
        return NULL;
    }
    if (1 != fread(buff, (size_t)len, 1, f)) {
        free(buff);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)len;
    return buff;
}

static bool app_write_file(const char* path, const uint8_t* buff, uint32_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    if (size && 1 != fwrite(buff, size, 1, f)) ok = false;
    if (0 != fclose(f)) ok = false;
    return ok;
}
#pragma endregion // file

static void app_free(app_t* app) {
    if (!app) return;
    if (app->reader) jxlw_reader_unref(app->reader);
    if (app->writer) jxlw_writer_unref(app->writer, NULL, NULL);
    app->reader = NULL;
    app->writer = NULL;
}

#pragma region reader
/**
 * Open the input file.
 *
 * The api is picked at random: from a file or from a memory buffer.
 */
static jxlw_reader_t* app_open_reader(app_t* app, bool from_memory) {
    if (!from_memory) {
        printf("reader: jxlw_reader_new_from_file\n");
        return jxlw_reader_new_from_file(app->opt.input);
    }
    printf("reader: jxlw_reader_new_from_memory\n");
    uint32_t size = 0;
    uint8_t* buff = app_read_file(app->opt.input, &size);
    if (!buff) {
        printf("cannot read %s\n", app->opt.input);
        return NULL;
    }
    jxlw_reader_t* rd = jxlw_reader_new_from_memory(buff, size);
    free(buff);
    return rd;
}
#pragma endregion // reader

#pragma region writer
/**
 * Write the result.
 *
 * The api is picked at random: to a file or to a memory buffer.
 */
static bool app_write_result(app_t* app, bool to_file) {
    if (!to_file) {
        printf("writer: jxlw_writer_unref_to_bytes\n");
        uint32_t size = 0;
        uint8_t* buff = jxlw_writer_unref_to_bytes(app->writer, &size);
        app->writer = NULL;   // unref_to_bytes 已经释放了 writer
        if (!buff || !size) {
            printf("cannot encode the picture: %s\n", jxlw_get_status_string(jxlw_get_last_error()));
            jxlw_free(buff);
            return false;
        }
        if (!app_write_file(app->opt.output, buff, size)) {
            printf("cannot write %s\n", app->opt.output);
            jxlw_free(buff);
            return false;
        }
        jxlw_free(buff);
        return true;
    }
    printf("writer: jxlw_writer_unref_to_file\n");
    jxlw_status_e st = jxlw_writer_unref_to_file(app->writer, app->opt.output);
    app->writer = NULL;   // unref_to_file 已经释放了 writer
    if (JXLW_STATUS_OK != st) {
        printf("cannot write %s: %s\n", app->opt.output, jxlw_get_status_string(jxlw_get_last_error()));
        return false;
    }
    return true;
}
#pragma endregion // writer

#pragma region verify
/**
 * Decode the result again, it has to be a valid picture of the same size.
 */
static bool app_verify(const app_t* app, uint32_t width, uint32_t height) {
    jxlw_reader_t* rd = jxlw_reader_new_from_file(app->opt.output);
    if (!rd) {
        printf("verify: cannot open %s: %s\n", app->opt.output, jxlw_get_status_string(jxlw_get_last_error()));
        return false;
    }
    jxlw_reader_args_t rargs;
    bool ok = jxlw_reader_get_args(rd, &rargs);
    if (!ok) {
        printf("verify: cannot decode %s: %s\n", app->opt.output, jxlw_get_status_string(jxlw_get_last_error()));
    }
    else if (rargs.width != width || rargs.height != height) {
        printf("verify: the size is wrong: %ux%u, expected %ux%u\n", rargs.width, rargs.height, width, height);
        ok = false;
    }
    else {
        printf("verify: ok (%s, %ux%u, %s)\n", app_type_name(rargs.type), rargs.width, rargs.height, app_format_name(rargs.format));
    }
    jxlw_reader_unref(rd);
    return ok;
}
#pragma endregion // verify

/**
 * Copy the frames of the reader into the writer.
 */
static bool app_add_frames(app_t* app, const jxlw_reader_args_t* rargs, jxlw_codec_type_e type) {
    // jpeg 只有一张图
    const uint32_t frames = (JXLW_CODEC_TYPE_JPEG == type) ? 1 : rargs->frames;
    for (uint32_t i = 0; i < frames; i++) {
        jxlw_frame_args_t fargs;
        uint8_t* pixel = jxlw_reader_read_frame(app->reader, i, &fargs);
        if (!pixel) {
            printf("cannot read the frame %u: %s\n", i, jxlw_get_status_string(jxlw_get_last_error()));
            return false;
        }
        jxlw_status_e st = jxlw_writer_add_frame(app->writer, &fargs, pixel);
        jxlw_free(pixel);
        if (JXLW_STATUS_OK != st) {
            printf("cannot add the frame %u: %s\n", i, jxlw_get_status_string(jxlw_get_last_error()));
            return false;
        }
    }
    return true;
}

/**
 * Decode the input and write it again in the other format.
 */
static bool app_run_convert(app_t* app) {
    const bool from_memory = app_rand_bool();
    const bool to_file = app_rand_bool();

    app->reader = app_open_reader(app, from_memory);
    if (!app->reader) {
        printf("cannot open %s: %s\n", app->opt.input, jxlw_get_status_string(jxlw_get_last_error()));
        return false;
    }

    jxlw_reader_args_t rargs;
    if (!jxlw_reader_get_args(app->reader, &rargs)) {
        printf("cannot decode %s: %s\n", app->opt.input, jxlw_get_status_string(jxlw_get_last_error()));
        goto failed;
    }
    printf("%s: %s, %ux%u, %u frame(s), %s\n", app->opt.input, app_type_name(rargs.type),
           rargs.width, rargs.height, rargs.frames, app_format_name(rargs.format));

    jxlw_writer_args_t wargs;
    memset(&wargs, 0, sizeof(wargs));
    // 输入的像素格式和解码出来的一致，jxl 会按这个设置来编码
    wargs.format = rargs.format;
    // 默认转成另一种格式
    wargs.type = (JXLW_CODEC_TYPE_JXL == rargs.type) ? JXLW_CODEC_TYPE_JPEG : JXLW_CODEC_TYPE_JXL;
    if (JXLW_CODEC_TYPE_UNKNOW != app->opt.target)
        wargs.type = app->opt.target;
    wargs.quality = (jxlw_writer_quality_e)app->opt.quality;
    // 每帧的时长
    wargs.duration = rargs.frames ? (rargs.duration / rargs.frames) : 100;
    if (!wargs.duration) wargs.duration = 100;

    app->writer = jxlw_writer_new(&wargs);
    if (!app->writer) {
        printf("cannot create the writer: %s\n", jxlw_get_status_string(jxlw_get_last_error()));
        goto failed;
    }

    if (!app_add_frames(app, &rargs, wargs.type)) goto failed;

    // 输出路径
    if (!app->opt.have_output) {
        if (!app_make_output(app->opt.input, JXLW_CODEC_TYPE_JXL == wargs.type, app->opt.output, sizeof(app->opt.output))) {
            printf("cannot build the output path\n");
            goto failed;
        }
    }
    if (!app_write_result(app, to_file)) goto failed;
    printf("%s written\n", app->opt.output);

    if (app->opt.verify && !app_verify(app, rargs.width, rargs.height)) goto failed;

    // 成功了就释放掉
    jxlw_reader_unref(app->reader);
    app->reader = NULL;
    return true;

failed:
    if (app->reader) {
        jxlw_reader_unref(app->reader);
        app->reader = NULL;
    }
    if (app->writer) {
        jxlw_writer_unref(app->writer, NULL, NULL);
        app->writer = NULL;
    }
    return false;
}

#pragma region args
static void app_print_usage() {
    printf("jxlw_cli - convert a JPEG / JPEG XL picture into the other format\n"
           "\n"
           "  jxlw_cli [options] [input.jpg|input.jxl]\n"
           "\n"
           "  -i, --input <file>    the input file, when it is missing a dialog is shown\n"
           "  -o, --output <file>   the output file, by default the extension of the input is swapped\n"
           "  -t, --type <jxl|jpeg> the format of the result, by default the other one\n"
           "  -q, --quality <0..5>  the quality: 0 lossless, 1 lowest, 2 low, 3 default,\n"
           "                        4 height, 5 heigest\n"
           "  -s, --seed <n>        the seed of the random generator\n"
           "  -n, --count <n>       how many times the conversion is done (default: 1)\n"
           "      --no-dialog       never show the file dialog\n"
           "      --no-verify       do not decode the result again\n"
           "  -h, --help            this text\n");
}

static bool app_parse_type(const char* text, jxlw_codec_type_e* type) {
    if (!text) return false;
    if (app_str_ieq(text, "jxl") || app_str_ieq(text, ".jxl")) {
        *type = JXLW_CODEC_TYPE_JXL;
        return true;
    }
    if (app_str_ieq(text, "jpeg") || app_str_ieq(text, "jpg") || app_str_ieq(text, ".jpg") || app_str_ieq(text, ".jpeg")) {
        *type = JXLW_CODEC_TYPE_JPEG;
        return true;
    }
    return false;
}

// 从命令行里面取一个参数，idx 指向选项本身，参数是它的下一个
static const char* app_next_arg(int argc, char** argv, int* idx, const char* name) {
    if (*idx + 1 >= argc) {
        printf("%s: the argument is missing\n", name);
        return NULL;
    }
    (*idx)++;
    return argv[*idx];
}

static int app_parse_args(app_options_t* opt, int argc, char** argv) {
    int i = 1;
    for (; i < argc; i++) {
        const char* arg = argv[i];
        if (0 == strcmp(arg, "-h") || 0 == strcmp(arg, "--help")) {
            app_print_usage();
            return -1;
        }
        else if (0 == strcmp(arg, "-i") || 0 == strcmp(arg, "--input")) {
            const char* val = app_next_arg(argc, argv, &i, arg);
            if (!val) return -1;
            app_strcpy(opt->input, sizeof(opt->input), val);
        }
        else if (0 == strcmp(arg, "-o") || 0 == strcmp(arg, "--output")) {
            const char* val = app_next_arg(argc, argv, &i, arg);
            if (!val) return -1;
            app_strcpy(opt->output, sizeof(opt->output), val);
            opt->have_output = true;
        }
        else if (0 == strcmp(arg, "-t") || 0 == strcmp(arg, "--type")) {
            const char* val = app_next_arg(argc, argv, &i, arg);
            if (!val || !app_parse_type(val, &opt->target)) {
                printf("%s: unknown format\n", arg);
                return -1;
            }
        }
        else if (0 == strcmp(arg, "-q") || 0 == strcmp(arg, "--quality")) {
            const char* val = app_next_arg(argc, argv, &i, arg);
            if (!val) return -1;
            const int quality = atoi(val);
            if (quality < 0 || quality >= (int)JXLW_WRITER_QUALITY_CNT) {
                printf("%s: the quality has to be between 0 and %d\n", arg, (int)JXLW_WRITER_QUALITY_CNT - 1);
                return -1;
            }
            opt->quality = quality;
        }
        else if (0 == strcmp(arg, "-s") || 0 == strcmp(arg, "--seed")) {
            const char* val = app_next_arg(argc, argv, &i, arg);
            if (!val) return -1;
            opt->seed = (uint32_t)strtoul(val, NULL, 0);
        }
        else if (0 == strcmp(arg, "-n") || 0 == strcmp(arg, "--count")) {
            const char* val = app_next_arg(argc, argv, &i, arg);
            if (!val) return -1;
            opt->count = (uint32_t)strtoul(val, NULL, 0);
            if (!opt->count) opt->count = 1;
        }
        else if (0 == strcmp(arg, "--no-dialog")) {
            opt->dialog = false;
        }
        else if (0 == strcmp(arg, "--no-verify")) {
            opt->verify = false;
        }
        else if (arg[0] == '-' && arg[1]) {
            printf("unknown option: %s\n", arg);
            return -1;
        }
        else {
            // 第一个不带选项的参数就是输入文件
            if (!opt->input[0]) {
                app_strcpy(opt->input, sizeof(opt->input), arg);
            }
            else {
                printf("too many input files: %s\n", arg);
                return -1;
            }
        }
    }
    return 0;
}
#pragma endregion // args

int main(int argc, char** argv) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.opt.quality = JXLW_WRITER_QUALITY_DEFAULT;
    app.opt.count = 1;
    app.opt.dialog = true;
    app.opt.verify = true;
    app.opt.target = JXLW_CODEC_TYPE_UNKNOW;
    app.opt.seed = (uint32_t)time(NULL);

    if (0 != app_parse_args(&app.opt, argc, argv)) return -1;

    // 没有输入文件的时候弹框选一个
    if (!app.opt.input[0] && app.opt.dialog) {
        const char* filter[] = { "*.jpg", "*.jpeg", "*.jxl" };
        const char* path = tinyfd_openFileDialog("Open image", NULL, 3, filter, "Image files", 0);
        if (path && path[0])
            app_strcpy(app.opt.input, sizeof(app.opt.input), path);
    }
    if (!app.opt.input[0]) {
        app_print_usage();
        return -1;
    }

    app_srand(app.opt.seed);
    printf("seed: %u\n", app.opt.seed);

    bool rev = true;
    for (uint32_t n = 0; n < app.opt.count; n++) {
        if (1 < app.opt.count) printf("--- run %u/%u ---\n", n + 1, app.opt.count);
        rev = app_run_convert(&app);
        if (!rev) break;
    }
    app_free(&app);
    return rev ? 0 : -1;
}
