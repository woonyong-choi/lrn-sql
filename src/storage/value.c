#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/value.h"

#define NUMBER_TEXT_BYTES 40u
#define TWO_POW_63 9223372036854775808.0

static bool utf8_valid(const uint8_t *bytes, size_t size)
{
    size_t i = 0;
    while (i < size) {
        uint8_t lead = bytes[i];
        size_t extra;
        uint32_t code;
        if (lead < 0x80) {
            i++;
            continue;
        }
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
            code = lead & 0x1fu;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2;
            code = lead & 0x0fu;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (size - i <= extra) return false;
        for (size_t k = 1; k <= extra; k++) {
            if ((bytes[i + k] & 0xc0u) != 0x80u) return false;
            code = code << 6 | (bytes[i + k] & 0x3fu);
        }
        if (extra == 2 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff))) return false;
        if (extra == 3 && (code < 0x10000 || code > 0x10ffff)) return false;
        i += extra + 1;
    }
    return true;
}

static value_status_t check_view(const value_view_t *view)
{
    if (view == NULL) return VALUE_INVALID;
    switch (view->type) {
        case VALUE_NULL:
        case VALUE_INTEGER:
        case VALUE_REAL:
            if (view->bytes != NULL || view->size_bytes != 0) return VALUE_INVALID;
            if (view->type == VALUE_REAL && !isfinite(view->real)) return VALUE_OVERFLOW;
            return VALUE_OK;
        case VALUE_TEXT:
        case VALUE_BLOB:
            if (view->size_bytes > VALUE_MAX_BYTES) return VALUE_LIMIT;
            if (view->size_bytes > 0 && view->bytes == NULL) return VALUE_INVALID;
            if (view->type == VALUE_TEXT && !utf8_valid(view->bytes, view->size_bytes)) {
                return VALUE_INVALID;
            }
            return VALUE_OK;
        default:
            return VALUE_INVALID;
    }
}

void value_clear(value_t *value)
{
    if (value == NULL) return;
    if (value->owns_bytes) free((void *)value->view.bytes);
    memset(value, 0, sizeof(*value));
}

value_view_t value_as_view(const value_t *value)
{
    return value->view;
}

/* 새 값을 다 만든 뒤에만 이전 값을 해제해 실패한 호출과 별칭 입력을 모두 안전하게 한다. */
static void install(value_t *out, value_t next)
{
    value_clear(out);
    *out = next;
}

static value_status_t make_bytes(value_type_t type, const uint8_t *bytes, size_t size,
                                 value_t *result)
{
    uint8_t *copy = NULL;
    if (size > 0) {
        copy = malloc(size);
        if (copy == NULL) return VALUE_NOMEM;
        memcpy(copy, bytes, size);
    }
    *result = (value_t){{type, 0, 0.0, copy, size}, copy != NULL};
    return VALUE_OK;
}

static value_status_t set_bytes(value_t *out, value_type_t type, const uint8_t *bytes,
                                size_t size)
{
    if (out == NULL) return VALUE_INVALID;
    value_view_t view = {type, 0, 0.0, bytes, size};
    value_status_t status = check_view(&view);
    if (status != VALUE_OK) return status;
    value_t next;
    status = make_bytes(type, bytes, size, &next);
    if (status == VALUE_OK) install(out, next);
    return status;
}

value_status_t value_set_null(value_t *out)
{
    if (out == NULL) return VALUE_INVALID;
    install(out, (value_t){{VALUE_NULL, 0, 0.0, NULL, 0}, false});
    return VALUE_OK;
}

value_status_t value_set_integer(value_t *out, int64_t integer)
{
    if (out == NULL) return VALUE_INVALID;
    install(out, (value_t){{VALUE_INTEGER, integer, 0.0, NULL, 0}, false});
    return VALUE_OK;
}

value_status_t value_set_real(value_t *out, double real)
{
    if (out == NULL) return VALUE_INVALID;
    if (!isfinite(real)) return VALUE_OVERFLOW;
    install(out, (value_t){{VALUE_REAL, 0, real, NULL, 0}, false});
    return VALUE_OK;
}

value_status_t value_set_text(value_t *out, const uint8_t *bytes, size_t size_bytes)
{
    return set_bytes(out, VALUE_TEXT, bytes, size_bytes);
}

value_status_t value_set_blob(value_t *out, const uint8_t *bytes, size_t size_bytes)
{
    return set_bytes(out, VALUE_BLOB, bytes, size_bytes);
}

value_status_t value_clone(const value_view_t *from, value_t *out)
{
    if (out == NULL) return VALUE_INVALID;
    value_status_t status = check_view(from);
    if (status != VALUE_OK) return status;
    if (from->type == VALUE_TEXT || from->type == VALUE_BLOB) {
        value_t next;
        status = make_bytes(from->type, from->bytes, from->size_bytes, &next);
        if (status == VALUE_OK) install(out, next);
        return status;
    }
    install(out, (value_t){*from, false});
    return VALUE_OK;
}

/* 로캘의 소수점 문자열을 '.'로 되돌려 출력이 로캘과 무관하게 한다. */
static void normalize_decimal_point(char *text)
{
    const struct lconv *conv = localeconv();
    const char *point = conv != NULL ? conv->decimal_point : ".";
    size_t point_length = strlen(point);
    if (point_length == 0 || strcmp(point, ".") == 0) return;
    char *found = strstr(text, point);
    if (found == NULL) return;
    *found = '.';
    memmove(found + 1, found + point_length, strlen(found + point_length) + 1);
}

static size_t render_number(const value_view_t *view, char text[NUMBER_TEXT_BYTES])
{
    if (view->type == VALUE_INTEGER) {
        uint64_t magnitude = view->integer < 0 ? 0 - (uint64_t)view->integer
                                               : (uint64_t)view->integer;
        char reversed[24];
        size_t digits = 0, length = 0;
        do {
            reversed[digits++] = (char)('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude > 0);
        if (view->integer < 0) text[length++] = '-';
        while (digits > 0) text[length++] = reversed[--digits];
        text[length] = '\0';
        return length;
    }
    /* 15·16·17자리 가운데 다시 읽어 같은 값이 되는 가장 짧은 표현. 17자리는 모든 유한값을 왕복한다 */
    for (int digits = 15; digits <= 17; digits++) {
        snprintf(text, NUMBER_TEXT_BYTES, "%.*g", digits, view->real);
        if (digits == 17 || strtod(text, NULL) == view->real) break;
    }
    normalize_decimal_point(text);
    if (strpbrk(text, ".eE") == NULL) strcat(text, ".0");
    return strlen(text);
}

static const char HEX_DIGITS[] = "0123456789abcdef";

static size_t formatted_size(const value_view_t *view)
{
    switch (view->type) {
        case VALUE_NULL: return 4;
        case VALUE_TEXT: return view->size_bytes;
        case VALUE_BLOB: return 3 + 2 * view->size_bytes;
        default: {
            char text[NUMBER_TEXT_BYTES];
            return render_number(view, text);
        }
    }
}

static void write_formatted(const value_view_t *view, char *buffer)
{
    char text[NUMBER_TEXT_BYTES];
    switch (view->type) {
        case VALUE_NULL:
            memcpy(buffer, "NULL", 4);
            return;
        case VALUE_TEXT:
            if (view->size_bytes > 0) memcpy(buffer, view->bytes, view->size_bytes);
            return;
        case VALUE_BLOB:
            buffer[0] = 'X';
            buffer[1] = '\'';
            for (size_t i = 0; i < view->size_bytes; i++) {
                buffer[2 + 2 * i] = HEX_DIGITS[view->bytes[i] >> 4];
                buffer[3 + 2 * i] = HEX_DIGITS[view->bytes[i] & 0x0f];
            }
            buffer[2 + 2 * view->size_bytes] = '\'';
            return;
        default:
            memcpy(buffer, text, render_number(view, text));
            return;
    }
}

value_status_t value_format(const value_view_t *value, char *buffer, size_t capacity,
                            size_t *needed)
{
    if (needed == NULL || (capacity > 0 && buffer == NULL)) return VALUE_INVALID;
    value_status_t status = check_view(value);
    if (status != VALUE_OK) return status;
    size_t length = formatted_size(value);
    *needed = length;
    if (capacity < length) return VALUE_LIMIT;
    if (length > 0) write_formatted(value, buffer);
    if (capacity > length) buffer[length] = '\0';
    return VALUE_OK;
}

static value_status_t integer_to_real(const value_view_t *from, value_t *out)
{
    double real = (double)from->integer;
    /* 2^63은 int64 밖이라 되돌려 비교하기 전에 걸러야 UB가 없다. */
    if (real >= TWO_POW_63 || (int64_t)real != from->integer) return VALUE_OVERFLOW;
    install(out, (value_t){{VALUE_REAL, 0, real, NULL, 0}, false});
    return VALUE_OK;
}

static value_status_t real_to_integer(const value_view_t *from, value_t *out)
{
    double real = from->real;
    if (real >= TWO_POW_63 || real < -TWO_POW_63) return VALUE_OVERFLOW;
    if (real != trunc(real)) return VALUE_OVERFLOW;
    install(out, (value_t){{VALUE_INTEGER, (int64_t)real, 0.0, NULL, 0}, false});
    return VALUE_OK;
}

static bool is_digit(uint8_t byte)
{
    return byte >= '0' && byte <= '9';
}

static value_status_t text_to_integer(const value_view_t *from, value_t *out)
{
    const uint8_t *bytes = from->bytes;
    size_t size = from->size_bytes, i = 0;
    bool negative = false;
    if (size > 0 && (bytes[0] == '+' || bytes[0] == '-')) {
        negative = bytes[0] == '-';
        i = 1;
    }
    if (i == size) return VALUE_INVALID;
    for (size_t k = i; k < size; k++) {
        if (!is_digit(bytes[k])) return VALUE_INVALID;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    for (; i < size; i++) {
        unsigned digit = bytes[i] - '0';
        if (magnitude > (limit - digit) / 10) return VALUE_OVERFLOW;
        magnitude = magnitude * 10 + digit;
    }
    int64_t integer = negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
    install(out, (value_t){{VALUE_INTEGER, integer, 0.0, NULL, 0}, false});
    return VALUE_OK;
}

/* [+-]?digits(.digits)?([eE][+-]?digits)? 전체인지 보고 가수에 0이 아닌 자릿수가 있는지 알린다. */
static bool real_grammar(const uint8_t *bytes, size_t size, bool *nonzero)
{
    size_t i = 0;
    *nonzero = false;
    if (i < size && (bytes[i] == '+' || bytes[i] == '-')) i++;
    size_t start = i;
    for (; i < size && is_digit(bytes[i]); i++) *nonzero = *nonzero || bytes[i] != '0';
    if (i == start) return false;
    if (i < size && bytes[i] == '.') {
        start = ++i;
        for (; i < size && is_digit(bytes[i]); i++) *nonzero = *nonzero || bytes[i] != '0';
        if (i == start) return false;
    }
    if (i < size && (bytes[i] == 'e' || bytes[i] == 'E')) {
        i++;
        if (i < size && (bytes[i] == '+' || bytes[i] == '-')) i++;
        start = i;
        for (; i < size && is_digit(bytes[i]); i++) {}
        if (i == start) return false;
    }
    return i == size;
}

static value_status_t text_to_real(const value_view_t *from, value_t *out)
{
    bool nonzero;
    if (!real_grammar(from->bytes, from->size_bytes, &nonzero)) return VALUE_INVALID;
    const struct lconv *conv = localeconv();
    const char *point = conv != NULL ? conv->decimal_point : ".";
    size_t point_length = strlen(point);
    char *copy = malloc(from->size_bytes + point_length + 1);
    if (copy == NULL) return VALUE_NOMEM;
    size_t length = 0;
    for (size_t i = 0; i < from->size_bytes; i++) {
        if (from->bytes[i] == '.') {
            memcpy(copy + length, point, point_length);
            length += point_length;
        } else {
            copy[length++] = (char)from->bytes[i];
        }
    }
    copy[length] = '\0';
    char *end;
    double real = strtod(copy, &end);
    bool consumed = end == copy + length;
    free(copy);
    if (!consumed) return VALUE_INVALID;
    if (!isfinite(real) || (real == 0.0 && nonzero)) return VALUE_OVERFLOW;
    install(out, (value_t){{VALUE_REAL, 0, real, NULL, 0}, false});
    return VALUE_OK;
}

static value_status_t number_to_text(const value_view_t *from, value_t *out)
{
    char text[NUMBER_TEXT_BYTES];
    size_t length = render_number(from, text);
    value_t next;
    value_status_t status = make_bytes(VALUE_TEXT, (const uint8_t *)text, length, &next);
    if (status == VALUE_OK) install(out, next);
    return status;
}

static value_status_t convert_bytes(const value_view_t *from, value_type_t target,
                                    value_t *out)
{
    value_t next;
    value_status_t status = make_bytes(target, from->bytes, from->size_bytes, &next);
    if (status == VALUE_OK) install(out, next);
    return status;
}

value_status_t value_cast(const value_view_t *from, value_type_t target, value_t *out)
{
    if (out == NULL) return VALUE_INVALID;
    value_status_t status = check_view(from);
    if (status != VALUE_OK) return status;
    if ((unsigned)target > (unsigned)VALUE_BLOB) return VALUE_INVALID;
    if (from->type == VALUE_NULL) return value_set_null(out);
    if (from->type == target) return value_clone(from, out);
    switch (from->type) {
        case VALUE_INTEGER:
            if (target == VALUE_REAL) return integer_to_real(from, out);
            if (target == VALUE_TEXT) return number_to_text(from, out);
            return VALUE_MISMATCH;
        case VALUE_REAL:
            if (target == VALUE_INTEGER) return real_to_integer(from, out);
            if (target == VALUE_TEXT) return number_to_text(from, out);
            return VALUE_MISMATCH;
        case VALUE_TEXT:
            if (target == VALUE_INTEGER) return text_to_integer(from, out);
            if (target == VALUE_REAL) return text_to_real(from, out);
            return convert_bytes(from, VALUE_BLOB, out);
        case VALUE_BLOB:
            if (target != VALUE_TEXT) return VALUE_MISMATCH;
            if (!utf8_valid(from->bytes, from->size_bytes)) return VALUE_INVALID;
            return convert_bytes(from, VALUE_TEXT, out);
        default:
            return VALUE_INVALID;
    }
}

static int rank(value_type_t type)
{
    switch (type) {
        case VALUE_NULL: return 0;
        case VALUE_INTEGER:
        case VALUE_REAL: return 1;
        case VALUE_TEXT: return 2;
        default: return 3;
    }
}

static int sign_of(bool less, bool greater)
{
    return less ? -1 : greater ? 1 : 0;
}

/* i와 d의 정확한 대소. d는 유한하다. */
static int compare_integer_real(int64_t integer, double real)
{
    if (real >= TWO_POW_63) return -1;
    if (real < -TWO_POW_63) return 1;
    double floored = floor(real);
    int64_t base = (int64_t)floored;
    if (integer != base) return sign_of(integer < base, integer > base);
    return real > floored ? -1 : 0;
}

static int compare_numbers(const value_view_t *left, const value_view_t *right)
{
    if (left->type == VALUE_INTEGER && right->type == VALUE_INTEGER) {
        return sign_of(left->integer < right->integer, left->integer > right->integer);
    }
    if (left->type == VALUE_REAL && right->type == VALUE_REAL) {
        return sign_of(left->real < right->real, left->real > right->real);
    }
    if (left->type == VALUE_INTEGER) return compare_integer_real(left->integer, right->real);
    return -compare_integer_real(right->integer, left->real);
}

static int compare_bytes(const value_view_t *left, const value_view_t *right)
{
    size_t common = left->size_bytes < right->size_bytes ? left->size_bytes : right->size_bytes;
    int result = common > 0 ? memcmp(left->bytes, right->bytes, common) : 0;
    if (result != 0) return result < 0 ? -1 : 1;
    return sign_of(left->size_bytes < right->size_bytes, left->size_bytes > right->size_bytes);
}

/* 유효한 UTF-8에서 이어 바이트가 아닌 바이트 하나가 문자 하나의 시작이다. */
static size_t utf8_char_count(const uint8_t *bytes, size_t size)
{
    size_t count = 0;
    for (size_t i = 0; i < size; i++) {
        if ((bytes[i] & 0xc0u) != 0x80u) count++;
    }
    return count;
}

value_status_t value_text_chars(const value_view_t *text, size_t *chars)
{
    if (chars == NULL) return VALUE_INVALID;
    value_status_t status = check_view(text);
    if (status != VALUE_OK) return status;
    if (text->type != VALUE_TEXT) return VALUE_MISMATCH;
    *chars = utf8_char_count(text->bytes, text->size_bytes);
    return VALUE_OK;
}

value_status_t value_check_varchar(const value_view_t *value, size_t max_chars)
{
    if (max_chars == 0) return VALUE_INVALID;
    value_status_t status = check_view(value);
    if (status != VALUE_OK) return status;
    if (value->type == VALUE_NULL) return VALUE_OK;
    if (value->type != VALUE_TEXT) return VALUE_MISMATCH;
    return utf8_char_count(value->bytes, value->size_bytes) <= max_chars ? VALUE_OK : VALUE_LIMIT;
}

value_status_t value_compare(const value_view_t *left, const value_view_t *right, int *order)
{
    if (order == NULL) return VALUE_INVALID;
    value_status_t status = check_view(left);
    if (status == VALUE_OK) status = check_view(right);
    if (status != VALUE_OK) return status;
    int left_rank = rank(left->type), right_rank = rank(right->type);
    if (left_rank != right_rank) {
        *order = left_rank < right_rank ? -1 : 1;
    } else if (left_rank == 0) {
        *order = 0;
    } else if (left_rank == 1) {
        *order = compare_numbers(left, right);
    } else {
        *order = compare_bytes(left, right);
    }
    return VALUE_OK;
}
