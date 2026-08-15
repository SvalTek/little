#include "little.h"
#include "webui.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#include <pthread.h>
#include <unistd.h>
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct WebuiBinding {
    size_t bind_id;
    lt_Value callback;
} WebuiBinding;

typedef struct WebuiArg {
    char* string_value;
    long long int int_value;
    double float_value;
    uint8_t bool_value;
} WebuiArg;

typedef struct WebuiQueuedEvent {
    size_t bind_id;
    size_t window;
    size_t event_number;
    size_t event_type;
    size_t client_id;
    size_t connection_id;
    char* element;
    char* cookies;
    size_t argc;
    WebuiArg* args;
    uint8_t close_client_after_response;
    uint8_t close_after_response;
    uint8_t destroy_after_response;
    uint8_t exit_after_response;
    struct WebuiQueuedEvent* next;
} WebuiQueuedEvent;

typedef struct JsonParser {
    const char* text;
    uint32_t pos;
    uint8_t failed;
} JsonParser;

typedef struct JsonWriter {
    char* data;
    uint32_t length;
    uint32_t capacity;
    uint8_t failed;
} JsonWriter;

static const lt_Api* lt = 0;
static lt_VM* bound_vm = 0;
static lt_Value module_value = LT_VALUE_NULL;
static lt_Value callback_registry = LT_VALUE_NULL;
static WebuiBinding* bindings = 0;
static uint32_t binding_count = 0;
static uint32_t binding_capacity = 0;
static WebuiQueuedEvent* queue_head = 0;
static WebuiQueuedEvent* queue_tail = 0;
static WebuiQueuedEvent* current_dispatch_event = 0;

#ifdef _WIN32
static CRITICAL_SECTION queue_lock;
static uint8_t queue_lock_initialized = 0;
#else
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void lock_queue(void)
{
#ifdef _WIN32
    EnterCriticalSection(&queue_lock);
#else
    pthread_mutex_lock(&queue_lock);
#endif
}

static void unlock_queue(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&queue_lock);
#else
    pthread_mutex_unlock(&queue_lock);
#endif
}

static char* copy_string(const char* text)
{
    if (!text) text = "";
    size_t len = strlen(text);
    char* copy = malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, text, len + 1);
    return copy;
}

static WebuiBinding* find_binding(size_t bind_id)
{
    for (uint32_t i = 0; i < binding_count; ++i)
    {
        if (bindings[i].bind_id == bind_id) return &bindings[i];
    }
    return 0;
}

static uint8_t add_binding(size_t bind_id, lt_Value callback)
{
    WebuiBinding* existing = find_binding(bind_id);
    if (existing)
    {
        existing->callback = callback;
        return 1;
    }

    if (binding_count >= binding_capacity)
    {
        uint32_t capacity = binding_capacity == 0 ? 8 : binding_capacity * 2;
        WebuiBinding* next = realloc(bindings, sizeof(WebuiBinding) * capacity);
        if (!next) return 0;
        bindings = next;
        binding_capacity = capacity;
    }

    bindings[binding_count].bind_id = bind_id;
    bindings[binding_count].callback = callback;
    binding_count++;
    return 1;
}

static void free_event(WebuiQueuedEvent* event)
{
    if (!event) return;
    free(event->element);
    free(event->cookies);
    for (size_t i = 0; i < event->argc; ++i) free(event->args[i].string_value);
    free(event->args);
    free(event);
}

static void queue_event(webui_event_t* e)
{
    WebuiQueuedEvent* event = calloc(1, sizeof(WebuiQueuedEvent));
    if (!event) return;

    event->bind_id = e->bind_id;
    event->window = e->window;
    event->event_number = e->event_number;
    event->event_type = e->event_type;
    event->client_id = e->client_id;
    event->connection_id = e->connection_id;
    event->element = copy_string(e->element);
    event->cookies = copy_string(e->cookies);
    event->argc = webui_get_count(e);

    if (event->argc > 0)
    {
        event->args = calloc(event->argc, sizeof(WebuiArg));
        if (!event->args)
        {
            free_event(event);
            return;
        }
        for (size_t i = 0; i < event->argc; ++i)
        {
            event->args[i].string_value = copy_string(webui_get_string_at(e, i));
            event->args[i].int_value = webui_get_int_at(e, i);
            event->args[i].float_value = webui_get_float_at(e, i);
            event->args[i].bool_value = webui_get_bool_at(e, i) ? 1 : 0;
        }
    }

    lock_queue();
    if (queue_tail) queue_tail->next = event;
    else queue_head = event;
    queue_tail = event;
    unlock_queue();
}

static void webui_little_callback(webui_event_t* e)
{
    queue_event(e);
}

static WebuiQueuedEvent* pop_event(void)
{
    lock_queue();
    WebuiQueuedEvent* event = queue_head;
    if (event)
    {
        queue_head = event->next;
        if (!queue_head) queue_tail = 0;
        event->next = 0;
    }
    unlock_queue();
    return event;
}

static void json_skip_ws(JsonParser* parser)
{
    while (isspace((unsigned char)parser->text[parser->pos])) parser->pos++;
}

static uint8_t json_match(JsonParser* parser, const char* text)
{
    uint32_t len = (uint32_t)strlen(text);
    if (strncmp(parser->text + parser->pos, text, len) != 0) return 0;
    parser->pos += len;
    return 1;
}

static uint32_t json_hex_value(char c)
{
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
    return 0xFFFFFFFFu;
}

static uint32_t json_parse_hex4(JsonParser* parser)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < 4; ++i)
    {
        uint32_t digit = json_hex_value(parser->text[parser->pos]);
        if (digit == 0xFFFFFFFFu)
        {
            parser->failed = 1;
            return 0;
        }
        parser->pos++;
        value = (value << 4) | digit;
    }
    return value;
}

static char* json_parse_string_raw(lt_VM* vm, JsonParser* parser)
{
    if (parser->text[parser->pos] != '"')
    {
        parser->failed = 1;
        return 0;
    }
    parser->pos++;

    uint32_t length = 0;
    uint32_t capacity = 32;
    char* out = lt->alloc(vm, capacity);
    if (!out)
    {
        parser->failed = 1;
        return 0;
    }

    while (!parser->failed && parser->text[parser->pos] && parser->text[parser->pos] != '"')
    {
        unsigned char ch = (unsigned char)parser->text[parser->pos++];
        if (ch < 0x20)
        {
            parser->failed = 1;
            break;
        }

        if (ch == '\\')
        {
            ch = (unsigned char)parser->text[parser->pos++];
            if (ch == 0)
            {
                parser->failed = 1;
                break;
            }
            switch (ch)
            {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
            {
                uint32_t cp = json_parse_hex4(parser);
                if (parser->failed) break;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    /* High surrogate: expect a following \uXXXX low surrogate. */
                    if (parser->text[parser->pos] == '\\' && parser->text[parser->pos + 1] == 'u')
                    {
                        parser->pos += 2;
                        uint32_t low = json_parse_hex4(parser);
                        if (parser->failed) break;
                        if (low >= 0xDC00 && low <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        else cp = 0xFFFD; /* malformed pair: replacement char */
                    }
                    else cp = 0xFFFD; /* lone high surrogate: replacement char */
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD; /* lone low surrogate */

                if (cp == 0)
                {
                    /* Little strings are NUL-terminated and cannot represent U+0000. */
                    parser->failed = 1;
                    break;
                }

                if (length + 4 >= capacity)
                {
                    capacity *= 2;
                    char* next = lt->alloc(vm, capacity);
                    if (!next)
                    {
                        parser->failed = 1;
                        break;
                    }
                    memcpy(next, out, length);
                    lt->free(vm, out);
                    out = next;
                }
                if (cp <= 0x7F) out[length++] = (char)cp;
                else if (cp <= 0x7FF)
                {
                    out[length++] = (char)(0xC0 | (cp >> 6));
                    out[length++] = (char)(0x80 | (cp & 0x3F));
                }
                else if (cp <= 0xFFFF)
                {
                    out[length++] = (char)(0xE0 | (cp >> 12));
                    out[length++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[length++] = (char)(0x80 | (cp & 0x3F));
                }
                else
                {
                    out[length++] = (char)(0xF0 | (cp >> 18));
                    out[length++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    out[length++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[length++] = (char)(0x80 | (cp & 0x3F));
                }
                continue;
            }
            default:
                parser->failed = 1;
                break;
            }
        }

        if (parser->failed) break;
        if (length + 2 >= capacity)
        {
            capacity *= 2;
            char* next = lt->alloc(vm, capacity);
            if (!next)
            {
                parser->failed = 1;
                break;
            }
            memcpy(next, out, length);
            lt->free(vm, out);
            out = next;
        }
        out[length++] = (char)ch;
    }

    if (parser->failed || parser->text[parser->pos] != '"')
    {
        lt->free(vm, out);
        parser->failed = 1;
        return 0;
    }

    parser->pos++;
    out[length] = 0;
    return out;
}

static lt_Value json_parse_value(lt_VM* vm, JsonParser* parser, uint8_t depth);

static lt_Value json_parse_array(lt_VM* vm, JsonParser* parser, uint8_t depth)
{
    parser->pos++;
    lt_Value array = lt->make_array(vm);
    json_skip_ws(parser);
    if (parser->text[parser->pos] == ']')
    {
        parser->pos++;
        return array;
    }

    while (!parser->failed)
    {
        lt->array_push(vm, array, json_parse_value(vm, parser, depth + 1));
        json_skip_ws(parser);
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

static lt_Value json_parse_object(lt_VM* vm, JsonParser* parser, uint8_t depth)
{
    parser->pos++;
    lt_Value object = lt->make_table(vm);
    json_skip_ws(parser);
    if (parser->text[parser->pos] == '}')
    {
        parser->pos++;
        return object;
    }

    while (!parser->failed)
    {
        json_skip_ws(parser);
        char* key_text = json_parse_string_raw(vm, parser);
        if (!key_text) return LT_VALUE_NULL;
        lt_Value key = lt->make_string(vm, key_text);
        lt->free(vm, key_text);

        json_skip_ws(parser);
        if (parser->text[parser->pos] != ':')
        {
            parser->failed = 1;
            return LT_VALUE_NULL;
        }
        parser->pos++;

        lt->table_set(vm, object, key, json_parse_value(vm, parser, depth + 1));
        json_skip_ws(parser);
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

static lt_Value json_parse_number(JsonParser* parser)
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

static lt_Value json_parse_value(lt_VM* vm, JsonParser* parser, uint8_t depth)
{
    if (depth > 64)
    {
        parser->failed = 1;
        return LT_VALUE_NULL;
    }

    json_skip_ws(parser);
    char ch = parser->text[parser->pos];
    if (ch == '"')
    {
        char* text = json_parse_string_raw(vm, parser);
        if (!text) return LT_VALUE_NULL;
        lt_Value value = lt->make_string(vm, text);
        lt->free(vm, text);
        return value;
    }
    if (ch == '{') return json_parse_object(vm, parser, depth);
    if (ch == '[') return json_parse_array(vm, parser, depth);
    if (ch == '-' || isdigit((unsigned char)ch)) return json_parse_number(parser);
    if (json_match(parser, "true")) return LT_VALUE_TRUE;
    if (json_match(parser, "false")) return LT_VALUE_FALSE;
    if (json_match(parser, "null")) return LT_VALUE_NULL;

    parser->failed = 1;
    return LT_VALUE_NULL;
}

static lt_Value parse_payload(lt_VM* vm, const char* text)
{
    if (!text || !*text) return LT_VALUE_NULL;
    JsonParser parser = { text, 0, 0 };
    lt_Value value = json_parse_value(vm, &parser, 0);
    json_skip_ws(&parser);
    if (parser.failed || parser.text[parser.pos] != 0) return LT_VALUE_NULL;
    return value;
}

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

static char hex_digit(uint32_t value)
{
    return value < 10 ? (char)('0' + value) : (char)('a' + value - 10);
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
    uint64_t cursor = 0;
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
        double number = lt->get_number(value);
        if (!isfinite(number))
        {
            /* JSON has no representation for inf/nan; emit null instead. */
            writer_text(vm, writer, "null");
        }
        else
        {
            char scratch[64];
            snprintf(scratch, sizeof(scratch), "%.17g", number);
            writer_text(vm, writer, scratch);
        }
    }
    else if (LT_IS_STRING(value)) stringify_string(vm, writer, lt->get_string(vm, value));
    else if (LT_IS_ARRAY(value)) return stringify_array(vm, writer, value, depth);
    else if (LT_IS_TABLE(value)) return stringify_table(vm, writer, value, depth);
    else return 0;
    return !writer->failed;
}

static char* stringify_response(lt_VM* vm, lt_Value value)
{
    JsonWriter writer = { 0 };
    if (!stringify_value(vm, &writer, value, 0))
    {
        if (writer.data) lt->free(vm, writer.data);
        writer.data = 0;
        writer.length = 0;
        writer.capacity = 0;
        writer.failed = 0;
        writer_text(vm, &writer, "null");
    }
    writer_push(vm, &writer, 0);
    if (writer.failed) return 0;
    return writer.data;
}

static lt_Value await_promise_response(lt_VM* vm, lt_Value value)
{
    if (!lt->is_promise || !lt->is_promise(value)) return value;
    while (lt->promise_state(value) == LT_PROMISE_PENDING)
    {
        if (!lt->poll(vm)) break;
    }
    if (lt->promise_state(value) != LT_PROMISE_FULFILLED) return LT_VALUE_NULL;
    return lt->promise_result(value);
}

static lt_Value make_arg(lt_VM* vm, WebuiArg* arg)
{
    lt_Value table = lt->make_table(vm);
    lt->table_set(vm, table, lt->make_string(vm, "string"), lt->make_string(vm, arg->string_value ? arg->string_value : ""));
    lt->table_set(vm, table, lt->make_string(vm, "int"), lt->make_number((double)arg->int_value));
    lt->table_set(vm, table, lt->make_string(vm, "float"), lt->make_number(arg->float_value));
    lt->table_set(vm, table, lt->make_string(vm, "bool"), arg->bool_value ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return table;
}

static lt_Value make_event(lt_VM* vm, WebuiQueuedEvent* queued)
{
    lt_Value event = lt->make_table(vm);
    lt_Value args = lt->make_array(vm);
    for (size_t i = 0; i < queued->argc; ++i)
        lt->array_push(vm, args, make_arg(vm, &queued->args[i]));

    lt->table_set(vm, event, lt->make_string(vm, "window"), lt->make_number((double)queued->window));
    lt->table_set(vm, event, lt->make_string(vm, "eventNumber"), lt->make_number((double)queued->event_number));
    lt->table_set(vm, event, lt->make_string(vm, "eventType"), lt->make_number((double)queued->event_type));
    lt->table_set(vm, event, lt->make_string(vm, "clientId"), lt->make_number((double)queued->client_id));
    lt->table_set(vm, event, lt->make_string(vm, "connectionId"), lt->make_number((double)queued->connection_id));
    lt->table_set(vm, event, lt->make_string(vm, "element"), lt->make_string(vm, queued->element ? queued->element : ""));
    lt->table_set(vm, event, lt->make_string(vm, "cookies"), lt->make_string(vm, queued->cookies ? queued->cookies : ""));
    lt->table_set(vm, event, lt->make_string(vm, "args"), args);
    if (queued->argc > 0) lt->table_set(vm, event, lt->make_string(vm, "first"), lt->make_string(vm, queued->args[0].string_value ? queued->args[0].string_value : ""));
    else lt->table_set(vm, event, lt->make_string(vm, "first"), LT_VALUE_NULL);
    if (queued->argc == 1) lt->table_set(vm, event, lt->make_string(vm, "payload"), parse_payload(vm, queued->args[0].string_value));
    else lt->table_set(vm, event, lt->make_string(vm, "payload"), LT_VALUE_NULL);
    return event;
}

static uint32_t dispatch_events(lt_VM* vm)
{
    uint32_t dispatched = 0;
    WebuiQueuedEvent* event = 0;
    while ((event = pop_event()) != 0)
    {

        WebuiBinding* binding = find_binding(event->bind_id);
        if (binding)
        {
            lt_Value ev = make_event(vm, event);
            lt->push(vm, ev);
            WebuiQueuedEvent* previous_dispatch_event = current_dispatch_event;
            current_dispatch_event = event;
            uint16_t returns = lt->exec(vm, binding->callback, 1);
            current_dispatch_event = previous_dispatch_event;

            lt_Value response = LT_VALUE_NULL;
            if (returns > 0)
            {
                response = lt->pop(vm);
                returns--;
            }
            while (returns > 0)
            {
                lt->pop(vm);
                returns--;
            }

            if (event->event_type == WEBUI_EVENT_CALLBACK)
            {
                lt->push(vm, response);
                response = await_promise_response(vm, response);
                char* text = stringify_response(vm, response);
                webui_interface_set_response(event->window, event->event_number, text ? text : "null");
                if (text) lt->free(vm, text);
                lt->pop(vm);
            }
            if (event->close_client_after_response)
                webui_interface_close_client(event->window, event->event_number);
            if (event->close_after_response)
                webui_close(event->window);
            if (event->destroy_after_response)
                webui_destroy(event->window);
            if (event->exit_after_response)
                webui_exit();
            dispatched++;
        }
        free_event(event);
    }
    return dispatched;
}

static uint8_t expect_number(lt_VM* vm, lt_Value value, const char* message)
{
    if (LT_IS_NUMBER(value)) return 1;
    lt->runtime_error(vm, message);
    return 0;
}

static uint8_t expect_string(lt_VM* vm, lt_Value value, const char* message)
{
    if (LT_IS_STRING(value)) return 1;
    lt->runtime_error(vm, message);
    return 0;
}

static uint8_t expect_callable(lt_VM* vm, lt_Value value, const char* message)
{
    if (LT_IS_OBJECT(value))
    {
        lt_ObjectType type = LT_GET_OBJECT(value)->type;
        if (type == LT_OBJECT_FN || type == LT_OBJECT_CLOSURE || type == LT_OBJECT_NATIVEFN || type == LT_OBJECT_BOUND_NATIVE || type == LT_OBJECT_CLASS)
            return 1;
    }
    lt->runtime_error(vm, message);
    return 0;
}

static uint8_t native_new_window(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to webui.newWindow!");
    lt->push(vm, lt->make_number((double)webui_new_window()));
    return 1;
}

static uint8_t native_show(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected window and content for webui.show!");
    lt_Value content = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_string(vm, content, "Expected webui content to be string!");
    lt->push(vm, webui_show((size_t)lt->get_number(window), lt->get_string(vm, content)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_show_browser(lt_VM* vm, uint8_t argc)
{
    if (argc < 2 || argc > 3) lt->runtime_error(vm, "Expected window, content, and optional browser for webui.showBrowser!");
    lt_Value browser = argc == 3 ? lt->pop(vm) : lt->make_number((double)AnyBrowser);
    lt_Value content = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_string(vm, content, "Expected webui content to be string!");
    expect_number(vm, browser, "Expected webui browser to be number!");
    lt->push(vm, webui_show_browser((size_t)lt->get_number(window), lt->get_string(vm, content), (size_t)lt->get_number(browser)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_show_wv(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected window and content for webui.showWv!");
    lt_Value content = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_string(vm, content, "Expected webui content to be string!");
    lt->push(vm, webui_show_wv((size_t)lt->get_number(window), lt->get_string(vm, content)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_set_root_folder(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected window and path for webui.setRootFolder!");
    lt_Value path = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_string(vm, path, "Expected webui root folder path to be string!");
    lt->push(vm, webui_set_root_folder((size_t)lt->get_number(window), lt->get_string(vm, path)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_set_default_root_folder(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected path for webui.setDefaultRootFolder!");
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected webui default root folder path to be string!");
    lt->push(vm, webui_set_default_root_folder(lt->get_string(vm, path)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_bind(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected window, name, and callback for webui.bind!");
    lt_Value callback = lt->pop(vm);
    lt_Value name = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_string(vm, name, "Expected webui binding name to be string!");
    expect_callable(vm, callback, "Expected webui callback to be callable!");

    const char* binding_name = lt->get_string(vm, name);
    if (strcmp(binding_name, "*") == 0) binding_name = "";

    size_t bind_id = webui_bind((size_t)lt->get_number(window), binding_name, webui_little_callback);
    if (!add_binding(bind_id, callback)) lt->runtime_error(vm, "Unable to store webui callback!");
    lt->table_set(vm, callback_registry, lt->make_number((double)bind_id), callback);
    lt->push(vm, lt->make_number((double)bind_id));
    return 1;
}

static uint8_t native_poll(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to webui.poll!");
    lt->push(vm, lt->make_number((double)dispatch_events(vm)));
    return 1;
}

static uint8_t native_wait(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to webui.wait!");
    /* Ensure any queued events are processed before entering the wait loop */
    dispatch_events(vm);

    while (1)
    {
    /* Call non-blocking wait once --- it may return true to indicate
       event-loop work should continue, or false when there are no
       more servers / the wait should finish. Regardless, always
       attempt to dispatch queued events so the VM thread remains
       responsive to callbacks enqueued from worker threads. */
    int cont = webui_wait_async();

    /* Drain any events produced while the webui internals were running */
    dispatch_events(vm);

    if (!cont) break;

#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
    }

    return 0;
}

static uint8_t native_run(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected window and script for webui.run!");
    lt_Value script = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_string(vm, script, "Expected webui script to be string!");
    webui_run((size_t)lt->get_number(window), lt->get_string(vm, script));
    return 0;
}

static uint8_t native_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.close!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    size_t window_id = (size_t)lt->get_number(window);
    if (current_dispatch_event && current_dispatch_event->window == window_id)
        current_dispatch_event->close_after_response = 1;
    else
        webui_close(window_id);
    return 0;
}

static uint8_t native_close_client(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected event for webui.closeClient!");
    lt_Value event = lt->pop(vm);
    if (!LT_IS_TABLE(event)) lt->runtime_error(vm, "Expected event table for webui.closeClient!");
    lt_Value window = lt->table_get(vm, event, lt->make_string(vm, "window"));
    lt_Value event_number = lt->table_get(vm, event, lt->make_string(vm, "eventNumber"));
    expect_number(vm, window, "Expected webui event window to be number!");
    expect_number(vm, event_number, "Expected webui event number to be number!");
    size_t window_id = (size_t)lt->get_number(window);
    size_t event_id = (size_t)lt->get_number(event_number);
    if (current_dispatch_event &&
        current_dispatch_event->window == window_id &&
        current_dispatch_event->event_number == event_id)
        current_dispatch_event->close_client_after_response = 1;
    else
        webui_interface_close_client(window_id, event_id);
    return 0;
}

static uint8_t native_destroy(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.destroy!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    size_t window_id = (size_t)lt->get_number(window);
    if (current_dispatch_event && current_dispatch_event->window == window_id)
        current_dispatch_event->destroy_after_response = 1;
    else
        webui_destroy(window_id);
    return 0;
}

static uint8_t native_exit(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to webui.exit!");
    if (current_dispatch_event)
        current_dispatch_event->exit_after_response = 1;
    else
        webui_exit();
    return 0;
}

static uint8_t native_is_shown(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.isShown!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    lt->push(vm, webui_is_shown((size_t)lt->get_number(window)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_focus(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.focus!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    webui_focus((size_t)lt->get_number(window));
    return 0;
}

static uint8_t native_minimize(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.minimize!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    webui_minimize((size_t)lt->get_number(window));
    return 0;
}

static uint8_t native_maximize(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.maximize!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    webui_maximize((size_t)lt->get_number(window));
    return 0;
}

static uint8_t native_clean(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to webui.clean!");
    webui_clean();
    return 0;
}

static uint8_t native_set_size(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected window, width, and height for webui.setSize!");
    lt_Value height = lt->pop(vm);
    lt_Value width = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_number(vm, width, "Expected webui width to be number!");
    expect_number(vm, height, "Expected webui height to be number!");
    webui_set_size((size_t)lt->get_number(window), (unsigned int)lt->get_number(width), (unsigned int)lt->get_number(height));
    return 0;
}

static uint8_t native_set_center(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected window for webui.setCenter!");
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    webui_set_center((size_t)lt->get_number(window));
    return 0;
}

static uint8_t native_set_runtime(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected window and runtime for webui.setRuntime!");
    lt_Value runtime = lt->pop(vm);
    lt_Value window = lt->pop(vm);
    expect_number(vm, window, "Expected webui window to be number!");
    expect_number(vm, runtime, "Expected webui runtime to be number!");
    webui_set_runtime((size_t)lt->get_number(window), (size_t)lt->get_number(runtime));
    return 0;
}

static void set_native(lt_VM* vm, lt_Value module, const char* name, lt_NativeFn fn)
{
    lt->table_set(vm, module, lt->make_string(vm, name), lt->make_native(vm, fn));
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api))
        return LT_VALUE_NULL;

    /* WebUI owns process-global callback state, so it cannot safely serve two VMs. */
    if (bound_vm) return bound_vm == vm ? module_value : LT_VALUE_NULL;

    lt = api;
#ifdef _WIN32
    if (!queue_lock_initialized)
    {
        InitializeCriticalSection(&queue_lock);
        queue_lock_initialized = 1;
    }
#endif
    webui_set_config(asynchronous_response, true);
    bound_vm = vm;
    module_value = lt->make_table(vm);
    callback_registry = lt->make_table(vm);
    lt->table_set(vm, module_value, lt->make_string(vm, "__callbacks"), callback_registry);

    set_native(vm, module_value, "newWindow", native_new_window);
    set_native(vm, module_value, "show", native_show);
    set_native(vm, module_value, "showBrowser", native_show_browser);
    set_native(vm, module_value, "showWv", native_show_wv);
    set_native(vm, module_value, "setRootFolder", native_set_root_folder);
    set_native(vm, module_value, "setDefaultRootFolder", native_set_default_root_folder);
    set_native(vm, module_value, "bind", native_bind);
    set_native(vm, module_value, "poll", native_poll);
    set_native(vm, module_value, "wait", native_wait);
    set_native(vm, module_value, "run", native_run);
    set_native(vm, module_value, "close", native_close);
    set_native(vm, module_value, "closeClient", native_close_client);
    set_native(vm, module_value, "destroy", native_destroy);
    set_native(vm, module_value, "exit", native_exit);
    set_native(vm, module_value, "isShown", native_is_shown);
    set_native(vm, module_value, "focus", native_focus);
    set_native(vm, module_value, "minimize", native_minimize);
    set_native(vm, module_value, "maximize", native_maximize);
    set_native(vm, module_value, "clean", native_clean);
    set_native(vm, module_value, "setSize", native_set_size);
    set_native(vm, module_value, "setCenter", native_set_center);
    set_native(vm, module_value, "setRuntime", native_set_runtime);

    lt->table_set(vm, module_value, lt->make_string(vm, "AnyBrowser"), lt->make_number((double)AnyBrowser));
    lt->table_set(vm, module_value, lt->make_string(vm, "NoBrowser"), lt->make_number((double)NoBrowser));
    lt->table_set(vm, module_value, lt->make_string(vm, "Chrome"), lt->make_number((double)Chrome));
    lt->table_set(vm, module_value, lt->make_string(vm, "Firefox"), lt->make_number((double)Firefox));
    lt->table_set(vm, module_value, lt->make_string(vm, "Edge"), lt->make_number((double)Edge));
    lt->table_set(vm, module_value, lt->make_string(vm, "Webview"), lt->make_number((double)Webview));
    lt->table_set(vm, module_value, lt->make_string(vm, "RuntimeNone"), lt->make_number((double)None));
    lt->table_set(vm, module_value, lt->make_string(vm, "Deno"), lt->make_number((double)Deno));
    lt->table_set(vm, module_value, lt->make_string(vm, "NodeJS"), lt->make_number((double)NodeJS));
    lt->table_set(vm, module_value, lt->make_string(vm, "Bun"), lt->make_number((double)Bun));

    return module_value;
}
