/**
 * @file    test_webvtt.c
 * @brief   WebVTT 解析：时间轴、标签/实体清理、坏输入拒绝。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#include "../../src/media/qlwebvtt.h"

typedef struct {
    int count;
    int64_t start[8], end[8];
    char text[8][64];
} Cues;

static void on_cue(void *user, int64_t start, int64_t end, const char *text,
                   size_t len)
{
    Cues *c = (Cues *)user;
    if (c->count >= 8)
        return;
    const int i = c->count++;
    c->start[i] = start;
    c->end[i] = end;
    if (len >= sizeof(c->text[i]))
        len = sizeof(c->text[i]) - 1;
    memcpy(c->text[i], text, len);
    c->text[i][len] = '\0';
}

QL_TEST_BEGIN("test_webvtt")

    Cues c;
    memset(&c, 0, sizeof(c));

    const char *vtt =
        "WEBVTT - demo\r\n"
        "\r\n"
        "NOTE 这是注释，不应成为字幕\r\n"
        "\r\n"
        "STYLE\r\n"
        "::cue { color: yellow }\r\n"
        "\r\n"
        "intro\r\n"
        "00:00.500 --> 00:02.000 align:start\r\n"
        "Hello <b>world</b> &amp; friends\r\n"
        "\r\n"
        "00:02.500 --> 00:03.000\r\n"
        "第二行\r\n"
        "第二句\r\n";

    QL_CHECK_EQ(ql_vtt_parse(vtt, strlen(vtt), on_cue, &c), 2);
    QL_CHECK_EQ(c.count, 2);
    QL_CHECK_EQ((int)c.start[0], 500);
    QL_CHECK_EQ((int)c.end[0], 2000);
    QL_CHECK_STR(c.text[0], "Hello world & friends");
    QL_CHECK_EQ((int)c.start[1], 2500);
    QL_CHECK_STR(c.text[1], "第二行\n第二句");

    /* BOM + hh:mm:ss.mmm */
    Cues d;
    memset(&d, 0, sizeof(d));
    const char *vtt2 =
        "\xEF\xBB\xBFWEBVTT\n\n"
        "01:02:03.250 --> 01:02:04.500\n"
        "later\n";
    QL_CHECK_EQ(ql_vtt_parse(vtt2, strlen(vtt2), on_cue, &d), 1);
    QL_CHECK_EQ((int)d.start[0], (1 * 3600 + 2 * 60 + 3) * 1000 + 250);
    QL_CHECK_STR(d.text[0], "later");

    /* 缺头 / 坏时间：明确失败 */
    QL_CHECK_EQ(ql_vtt_parse("hello\n", 6, on_cue, &d), -1);
    const char *bad = "WEBVTT\n\n00:00 --> nope\nx\n";
    QL_CHECK_EQ(ql_vtt_parse(bad, strlen(bad), on_cue, &d), -1);

QL_TEST_END()
