#include "skbd/json.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *file, int line, const char *text) {
    if (!cond) {
        fprintf(stderr, "fail %s:%d: %s\n", file, line, text);
        failures++;
    }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

static SkbdJson *parse_ok(const char *text) {
    SkbdJson *value = NULL;
    char err[128];
    int rc;

    memcpy(err, "keep", 5);
    rc = skbd_json_parse(text, strlen(text), &value, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(err, "keep") == 0);
    CHECK(value != NULL);
    return value;
}

static void parse_fail(const char *text, size_t len, const char *needle) {
    SkbdJson *value = (SkbdJson *)1;
    char err[128];
    int rc;

    err[0] = '\0';
    rc = skbd_json_parse(text, len, &value, err, sizeof err);
    CHECK(rc != 0);
    CHECK(value == NULL);
    CHECK(strstr(err, needle) != NULL);
}

static int nested_arrays(int depth) {
    char *text;
    SkbdJson *value = NULL;
    char err[64];
    size_t bytes;
    int rc;

    bytes = (size_t)depth * 2u;
    text = malloc(bytes);
    if (text == NULL) {
        CHECK(0);
        return -1;
    }
    memset(text, '[', (size_t)depth);
    memset(text + depth, ']', (size_t)depth);
    err[0] = '\0';
    rc = skbd_json_parse(text, bytes, &value, err, sizeof err);
    free(text);
    if (rc == 0) {
        CHECK(value != NULL);
        CHECK(value->kind == SKBD_JSON_ARRAY);
        skbd_json_free(value);
    } else {
        CHECK(value == NULL);
        CHECK(strstr(err, "too deep") != NULL);
    }
    return rc;
}

static void test_scalars(void) {
    SkbdJson *value;
    const unsigned char emoji[] = {0xF0, 0x9F, 0x98, 0x80, 0};

    value = parse_ok("null");
    CHECK(value->kind == SKBD_JSON_NULL);
    skbd_json_free(value);

    value = parse_ok(" true ");
    CHECK(value->kind == SKBD_JSON_BOOL);
    CHECK(value->boolean == 1);
    skbd_json_free(value);

    value = parse_ok("false");
    CHECK(value->kind == SKBD_JSON_BOOL);
    CHECK(value->boolean == 0);
    skbd_json_free(value);

    value = parse_ok("0");
    CHECK(value->kind == SKBD_JSON_NUMBER);
    CHECK(value->number_ok == 1);
    CHECK(value->number == 0);
    CHECK(strcmp(value->text, "0") == 0);
    skbd_json_free(value);

    value = parse_ok("-0");
    CHECK(value->number_ok == 1);
    CHECK(value->number == 0);
    CHECK(strcmp(value->text, "-0") == 0);
    skbd_json_free(value);

    value = parse_ok("-12");
    CHECK(value->number_ok == 1);
    CHECK(value->number == -12);
    skbd_json_free(value);

    value = parse_ok("9223372036854775807");
    CHECK(value->number_ok == 1);
    CHECK(value->number == LLONG_MAX);
    skbd_json_free(value);

    value = parse_ok("-9223372036854775808");
    CHECK(value->number_ok == 1);
    CHECK(value->number == LLONG_MIN);
    skbd_json_free(value);

    value = parse_ok("9223372036854775808");
    CHECK(value->number_ok == 0);
    CHECK(strcmp(value->text, "9223372036854775808") == 0);
    skbd_json_free(value);

    value = parse_ok("-9223372036854775809");
    CHECK(value->number_ok == 0);
    CHECK(strcmp(value->text, "-9223372036854775809") == 0);
    skbd_json_free(value);

    value = parse_ok("1.5");
    CHECK(value->number_ok == 0);
    CHECK(strcmp(value->text, "1.5") == 0);
    skbd_json_free(value);

    value = parse_ok("1e2");
    CHECK(value->number_ok == 0);
    CHECK(strcmp(value->text, "1e2") == 0);
    skbd_json_free(value);

    value = parse_ok("1.5e2");
    CHECK(value->number_ok == 0);
    CHECK(strcmp(value->text, "1.5e2") == 0);
    skbd_json_free(value);

    value = parse_ok("0.5E-1");
    CHECK(value->number_ok == 0);
    CHECK(strcmp(value->text, "0.5E-1") == 0);
    skbd_json_free(value);

    value = parse_ok("\"a\\nb\\u0041\"");
    CHECK(value->kind == SKBD_JSON_STRING);
    CHECK(strcmp(value->text, "a\nbA") == 0);
    skbd_json_free(value);

    value = parse_ok("\"\\uD83D\\uDE00\"");
    CHECK(value->kind == SKBD_JSON_STRING);
    CHECK(memcmp(value->text, emoji, 5) == 0);
    skbd_json_free(value);

    value = parse_ok("\"\"");
    CHECK(value->kind == SKBD_JSON_STRING);
    CHECK(value->text != NULL);
    CHECK(value->text[0] == '\0');
    skbd_json_free(value);
}

static void test_reject(void) {
    char one = '1';

    parse_fail("", 0, "truncated");
    parse_fail(NULL, 0, "truncated");
    parse_fail(NULL, 1, "invalid");
    parse_fail("{", 1, "truncated");
    parse_fail("\"abc", 4, "truncated");
    parse_fail("[1,", 3, "truncated");
    parse_fail("tru", 3, "truncated");
    parse_fail("-", 1, "truncated");
    parse_fail("1.", 2, "truncated");
    parse_fail("1e", 2, "truncated");
    parse_fail("1.5e", 4, "truncated");
    parse_fail("1.5e+", 5, "truncated");
    parse_fail("1.5ex", 5, "invalid");
    parse_fail("01", 2, "invalid");
    parse_fail("+1", 2, "invalid");
    parse_fail("[1,]", 4, "invalid");
    parse_fail("truee", 5, "invalid");
    parse_fail("1 2", 3, "invalid");
    parse_fail("\"\\uD800\"", 8, "invalid");
    parse_fail("\"\n\"", 3, "invalid");
    parse_fail(&one, SKBD_JSON_MAX_BYTES + 1, "too large");
    skbd_json_free(NULL);
    CHECK(skbd_json_parse("1", 1, NULL, NULL, 0) != 0);
}

static void test_containers(void) {
    SkbdJson *value;
    const SkbdJson *field;

    CHECK(nested_arrays(32) == 0);
    CHECK(nested_arrays(33) != 0);

    value = parse_ok("[]");
    CHECK(value->kind == SKBD_JSON_ARRAY);
    CHECK(value->len == 0);
    skbd_json_free(value);

    value = parse_ok("[1, 2, 3]");
    CHECK(value->len == 3);
    CHECK(value->children[2].number == 3);
    skbd_json_free(value);

    value = parse_ok("{\"A\":1,\"a\":2}");
    field = skbd_json_get(value, "a");
    CHECK(field != NULL);
    CHECK(field->number == 2);
    CHECK(skbd_json_get(value, "A")->number == 1);
    CHECK(skbd_json_get(value, "missing") == NULL);
    CHECK(skbd_json_get(value, NULL) == NULL);
    CHECK(skbd_json_get(NULL, "a") == NULL);
    skbd_json_free(value);

    value = parse_ok("{\"a\":1,\"a\":2}");
    CHECK(skbd_json_get(value, "a")->number == 1);
    skbd_json_free(value);

    value = parse_ok("{\"k\":[true, null]}");
    field = skbd_json_get(value, "k");
    CHECK(field->kind == SKBD_JSON_ARRAY);
    CHECK(field->len == 2);
    CHECK(field->children[0].boolean == 1);
    CHECK(field->children[1].kind == SKBD_JSON_NULL);
    skbd_json_free(value);
}

int main(void) {
    test_scalars();
    test_reject();
    test_containers();
    return failures == 0 ? 0 : 1;
}
