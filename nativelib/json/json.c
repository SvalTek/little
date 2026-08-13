#include "little.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    const char* text;
    uint32_t pos;
    uint8_t failed;
} JsonParser;

typedef struct {
    char* data;
    uint32_t length;
    uint32_t capacity;
    uint8_t failed;
} JsonWriter;

static const lt_Api* lt = 0;

static void writer_push(lt_VM* vm, JsonWriter* writer, char ch)
{
    if (writer->failed) return;
    if (writer->capacity <= writer->length + 1)
    {
        uint32_t capacity = writer->capacity == 0 ? 64 : writer->capacity * 2;
        char* data = lt->alloc(vm, capacity);
        if (!data)
        {
            writer->failed = 1;
            return;
        }
        if (writer->data)
        {
            memcpy(data, writer->data, writer->length);
            lt->free(vm, writer->data);
        }
        writer->data = data;
        writer->capacity = capacity;
    }
    writer->data[writer->length++] = ch;
}

static void writer_text(lt_VM* vm, JsonWriter* writer, const char* text)
{
    while (*text) writer_push(vm, writer, *text++);
}

static void skip_ws(JsonParser* parser)
{
    while (isspace((unsigned char)parser->text[parser->pos])) parser->pos++;
}

static uint8_t match_text(JsonParser* parser, const char* text)
{
    uint32_t len = (uint32_t)strlen(text);
    if (strncmp(parser->text + parser->pos, text, len) != 0) return 0;
    parser->pos += len;
    return 1;
}

static char hex_digit(uint32_t value)
{
    return value < 10 ? (char)('0' + value) : (char)('a' + value - 10);
}

static char* parse_string_raw(lt_VM* vm, JsonParser* parser)
{
    if (parser->text[parser->pos] != '"')
    {
        parser->failed = 1;
        return 0;
    }
    parser->pos++;

    JsonWriter writer = { 0 };
    while (parser->text[parser->pos] && parser->text[parser->pos] != '"')
    {
        unsigned char ch = (unsigned char)parser->text[parser->pos++];
        if (ch < 0x20)
        {
            parser->failed = 1;
            break;
        }

        if (ch != '\\')
        {
            writer_push(vm, &writer, (char)ch);
            continue;
        }

        ch = (unsigned char)parser->text[parser->pos++];
        switch (ch)
        {
        case '"': writer_push(vm, &writer, '"'); break;
        case '\\': writer_push(vm, &writer, '\\'); break;
        case '/': writer_push(vm, &writer, '/'); break;
        case 'b': writer_push(vm, &writer, '\b'); break;
        case 'f': writer_push(vm, &writer, '\f'); break;
        case 'n': writer_push(vm, &writer, '\n'); break;
        case 'r': writer_push(vm, &writer, '\r'); break;
        case 't': writer_push(vm, &writer, '\t'); break;
        case 'u':
            writer_text(vm, &writer, "\\u");
            for (uint8_t i = 0; i < 4; ++i)
            {
                char hex = parser->text[parser->pos++];
                if (!isxdigit((unsigned char)hex))
                {
                    parser->failed = 1;
                    break;
                }
                writer_push(vm, &writer, hex);
            }
            break;
        default:
            parser->failed = 1;
            break;
        }
    }

    if (parser->failed || parser->text[parser->pos] != '"')
    {
        if (writer.data) lt->free(vm, writer.data);
        parser->failed = 1;
        return 0;
    }

    parser->pos++;
    writer_push(vm, &writer, 0);
    return writer.data;
}

static lt_Value parse_value(lt_VM* vm, JsonParser* parser, uint8_t depth);

static lt_Value parse_array(lt_VM* vm, JsonParser* parser, uint8_t depth)
{
    parser->pos++;
    lt_Value array = lt->make_array(vm);
    skip_ws(parser);
    if (parser->text[parser->pos] == ']')
    {
        parser->pos++;
        return array;
    }

    while (!parser->failed)
    {
        lt->array_push(vm, array, parse_value(vm, parser, depth + 1));
        skip_ws(parser);
        if (parser->text[parser->pos] == ']')
        {
            parser->pos++;
            return array;
        }
        if (parser->text[parser->pos] != ',')
        {
            parser->failed = 1;
            return LT_VALUE_NULL;
        }
        parser->pos++;
    }

    return LT_VALUE_NULL;
}

static lt_Value parse_object(lt_VM* vm, JsonParser* parser, uint8_t depth)
{
    parser->pos++;
    lt_Value object = lt->make_table(vm);
    skip_ws(parser);
    if (parser->text[parser->pos] == '}')
    {
        parser->pos++;
        return object;
    }

    while (!parser->failed)
    {
        skip_ws(parser);
        char* key_text = parse_string_raw(vm, parser);
        if (!key_text) return LT_VALUE_NULL;
        lt_Value key = lt->make_string(vm, key_text);
        lt->free(vm, key_text);

        skip_ws(parser);
        if (parser->text[parser->pos] != ':')
        {
            parser->failed = 1;
            return LT_VALUE_NULL;
        }
        parser->pos++;

        lt->table_set(vm, object, key, parse_value(vm, parser, depth + 1));
        skip_ws(parser);
        if (parser->text[parser->pos] == '}')
        {
            parser->pos++;
            return object;
        }
        if (parser->text[parser->pos] != ',')
        {
            parser->failed = 1;
            return LT_VALUE_NULL;
        }
        parser->pos++;
    }

    return LT_VALUE_NULL;
}

static lt_Value parse_number(JsonParser* parser)
{
    const char* start = parser->text + parser->pos;
    if (parser->text[parser->pos] == '-') parser->pos++;
    if (!isdigit((unsigned char)parser->text[parser->pos]))
    {
        parser->failed = 1;
        return LT_VALUE_NULL;
    }
    if (parser->text[parser->pos] == '0') parser->pos++;
    else while (isdigit((unsigned char)parser->text[parser->pos])) parser->pos++;

    if (parser->text[parser->pos] == '.')
    {
        parser->pos++;
        if (!isdigit((unsigned char)parser->text[parser->pos]))
        {
            parser->failed = 1;
            return LT_VALUE_NULL;
        }
        while (isdigit((unsigned char)parser->text[parser->pos])) parser->pos++;
    }

    if (parser->text[parser->pos] == 'e' || parser->text[parser->pos] == 'E')
    {
        parser->pos++;
        if (parser->text[parser->pos] == '+' || parser->text[parser->pos] == '-') parser->pos++;
        if (!isdigit((unsigned char)parser->text[parser->pos]))
        {
            parser->failed = 1;
            return LT_VALUE_NULL;
        }
        while (isdigit((unsigned char)parser->text[parser->pos])) parser->pos++;
    }

    double number = 0;
    sscanf(start, "%lf", &number);
    return lt->make_number(number);
}

static lt_Value parse_value(lt_VM* vm, JsonParser* parser, uint8_t depth)
{
    if (depth > 64)
    {
        parser->failed = 1;
        return LT_VALUE_NULL;
    }

    skip_ws(parser);
    char ch = parser->text[parser->pos];
    if (ch == '"')
    {
        char* text = parse_string_raw(vm, parser);
        if (!text) return LT_VALUE_NULL;
        lt_Value value = lt->make_string(vm, text);
        lt->free(vm, text);
        return value;
    }
    if (ch == '{') return parse_object(vm, parser, depth);
    if (ch == '[') return parse_array(vm, parser, depth);
    if (ch == '-' || isdigit((unsigned char)ch)) return parse_number(parser);
    if (match_text(parser, "true")) return LT_VALUE_TRUE;
    if (match_text(parser, "false")) return LT_VALUE_FALSE;
    if (match_text(parser, "null")) return LT_VALUE_NULL;

    parser->failed = 1;
    return LT_VALUE_NULL;
}

static void stringify_string(lt_VM* vm, JsonWriter* writer, const char* text)
{
    writer_push(vm, writer, '"');
    for (; *text; ++text)
    {
        unsigned char ch = (unsigned char)*text;
        switch (ch)
        {
        case '"': writer_text(vm, writer, "\\\""); break;
        case '\\': writer_text(vm, writer, "\\\\"); break;
        case '\b': writer_text(vm, writer, "\\b"); break;
        case '\f': writer_text(vm, writer, "\\f"); break;
        case '\n': writer_text(vm, writer, "\\n"); break;
        case '\r': writer_text(vm, writer, "\\r"); break;
        case '\t': writer_text(vm, writer, "\\t"); break;
        default:
            if (ch < 0x20)
            {
                writer_text(vm, writer, "\\u00");
                writer_push(vm, writer, hex_digit(ch >> 4));
                writer_push(vm, writer, hex_digit(ch & 0xF));
            }
            else writer_push(vm, writer, (char)ch);
            break;
        }
    }
    writer_push(vm, writer, '"');
}

static uint8_t stringify_value(lt_VM* vm, JsonWriter* writer, lt_Value value, uint8_t depth);

static uint8_t stringify_array(lt_VM* vm, JsonWriter* writer, lt_Value array, uint8_t depth)
{
    writer_push(vm, writer, '[');
    for (uint32_t i = 0; i < lt->array_length(array); ++i)
    {
        if (i > 0) writer_push(vm, writer, ',');
        if (!stringify_value(vm, writer, lt->array_get(vm, array, i), depth + 1)) return 0;
    }
    writer_push(vm, writer, ']');
    return 1;
}

static uint8_t stringify_table(lt_VM* vm, JsonWriter* writer, lt_Value table, uint8_t depth)
{
    uint8_t first = 1;
    uint32_t cursor = 0;
    lt_Value key = LT_VALUE_NULL;
    lt_Value value = LT_VALUE_NULL;

    writer_push(vm, writer, '{');
    while (lt->table_next(vm, table, &cursor, &key, &value))
    {
        if (!LT_IS_STRING(key)) return 0;
        if (!first) writer_push(vm, writer, ',');
        first = 0;
        stringify_string(vm, writer, lt->get_string(vm, key));
        writer_push(vm, writer, ':');
        if (!stringify_value(vm, writer, value, depth + 1)) return 0;
    }
    writer_push(vm, writer, '}');
    return 1;
}

static uint8_t stringify_value(lt_VM* vm, JsonWriter* writer, lt_Value value, uint8_t depth)
{
    if (depth > 64) return 0;
    if (LT_IS_NULL(value)) writer_text(vm, writer, "null");
    else if (LT_IS_TRUE(value)) writer_text(vm, writer, "true");
    else if (LT_IS_FALSE(value)) writer_text(vm, writer, "false");
    else if (LT_IS_NUMBER(value))
    {
        char scratch[64];
        snprintf(scratch, sizeof(scratch), "%.17g", lt->get_number(value));
        writer_text(vm, writer, scratch);
    }
    else if (LT_IS_STRING(value)) stringify_string(vm, writer, lt->get_string(vm, value));
    else if (LT_IS_ARRAY(value)) return stringify_array(vm, writer, value, depth);
    else if (LT_IS_TABLE(value)) return stringify_table(vm, writer, value, depth);
    else return 0;
    return !writer->failed;
}

static uint8_t json_parse_native(lt_VM* vm, uint8_t argc)
{
    if (argc != 1)
    {
        while (argc--) lt->pop(vm);
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value input = lt->pop(vm);
    if (!LT_IS_STRING(input))
    {
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    JsonParser parser = { lt->get_string(vm, input), 0, 0 };
    lt_Value value = parse_value(vm, &parser, 0);
    skip_ws(&parser);
    if (parser.failed || parser.text[parser.pos] != 0) value = LT_VALUE_NULL;
    lt->push(vm, value);
    return 1;
}

static uint8_t json_stringify_native(lt_VM* vm, uint8_t argc)
{
    if (argc != 1)
    {
        while (argc--) lt->pop(vm);
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value value = lt->pop(vm);
    JsonWriter writer = { 0 };
    if (!stringify_value(vm, &writer, value, 0))
    {
        if (writer.data) lt->free(vm, writer.data);
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }
    writer_push(vm, &writer, 0);
    lt_Value result = lt->make_string(vm, writer.data);
    lt->free(vm, writer.data);
    lt->push(vm, result);
    return 1;
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api))
        return LT_VALUE_NULL;

    lt = api;
    lt_Value module = lt->make_table(vm);
    lt->table_set(vm, module, lt->make_string(vm, "parse"), lt->make_native(vm, json_parse_native));
    lt->table_set(vm, module, lt->make_string(vm, "stringify"), lt->make_native(vm, json_stringify_native));
    return module;
}
