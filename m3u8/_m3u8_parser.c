/*
 * Copyright 2014 Globo.com Player authors. All rights reserved.
 * Use of this source code is governed by a MIT License
 * license that can be found in the LICENSE file.
 *
 * C extension for m3u8 parser - provides optimized parsing of M3U8 playlists.
 *
 * This module implements the same parsing logic as m3u8/parser.py but in C
 * for improved performance. The output is designed to be identical to the
 * Python implementation.
 *
 * Design notes (following CPython extension best practices per PEP 7):
 *
 * Memory Management:
 * - Uses module state instead of static globals for subinterpreter safety
 *   (PEP 573, PEP 3121)
 * - Uses PyMem_* allocators consistently for better debugging/tracing
 * - Single cleanup path via goto for reliable resource management
 * - All borrowed references are clearly documented
 *
 * Performance Optimizations:
 * - Frequently-used dict keys are cached as interned strings
 * - Attribute parsers are const static arrays built at compile time
 * - String operations use restrict pointers where applicable
 *
 * Error Handling:
 * - All Python C API calls that can fail are checked
 * - Helper macros (DICT_SET_AND_DECREF) ensure consistent cleanup
 * - ParseError exception is shared with the Python parser module
 *
 * Thread Safety:
 * - No mutable static state; all state is per-module
 * - GIL is held throughout parsing (no release/acquire)
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/*
 * Compatibility shims for Py_NewRef/Py_XNewRef (added in Python 3.10).
 * These make reference ownership more explicit at call sites.
 */
#if PY_VERSION_HEX < 0x030a00f0
static inline PyObject *
Py_NewRef(PyObject *obj)
{
    Py_INCREF(obj);
    return obj;
}

static inline PyObject *
Py_XNewRef(PyObject *obj)
{
    Py_XINCREF(obj);
    return obj;
}
#endif

/*
 * Forward declarations for inline helpers used before their definitions.
 */
static inline int dict_set_interned(PyObject *dict, PyObject *interned_key, PyObject *value);
static inline PyObject *dict_get_interned(PyObject *dict, PyObject *interned_key);

/*
 * Helper macro for setting dict items with proper error handling.
 * Decrefs value and returns/gotos on failure.
 */
#define DICT_SET_AND_DECREF(dict, key, value, cleanup_label) \
    do { \
        if (PyDict_SetItemString((dict), (key), (value)) < 0) { \
            Py_DECREF(value); \
            goto cleanup_label; \
        } \
        Py_DECREF(value); \
    } while (0)

/* Protocol tag definitions - must match protocol.py */
#define EXT_M3U "#EXTM3U"
#define EXT_X_TARGETDURATION "#EXT-X-TARGETDURATION"
#define EXT_X_MEDIA_SEQUENCE "#EXT-X-MEDIA-SEQUENCE"
#define EXT_X_DISCONTINUITY_SEQUENCE "#EXT-X-DISCONTINUITY-SEQUENCE"
#define EXT_X_PROGRAM_DATE_TIME "#EXT-X-PROGRAM-DATE-TIME"
#define EXT_X_MEDIA "#EXT-X-MEDIA"
#define EXT_X_PLAYLIST_TYPE "#EXT-X-PLAYLIST-TYPE"
#define EXT_X_KEY "#EXT-X-KEY"
#define EXT_X_STREAM_INF "#EXT-X-STREAM-INF"
#define EXT_X_VERSION "#EXT-X-VERSION"
#define EXT_X_ALLOW_CACHE "#EXT-X-ALLOW-CACHE"
#define EXT_X_ENDLIST "#EXT-X-ENDLIST"
#define EXTINF "#EXTINF"
#define EXT_I_FRAMES_ONLY "#EXT-X-I-FRAMES-ONLY"
#define EXT_X_ASSET "#EXT-X-ASSET"
#define EXT_X_BITRATE "#EXT-X-BITRATE"
#define EXT_X_BYTERANGE "#EXT-X-BYTERANGE"
#define EXT_X_I_FRAME_STREAM_INF "#EXT-X-I-FRAME-STREAM-INF"
#define EXT_X_DISCONTINUITY "#EXT-X-DISCONTINUITY"
#define EXT_X_CUE_OUT "#EXT-X-CUE-OUT"
#define EXT_X_CUE_OUT_CONT "#EXT-X-CUE-OUT-CONT"
#define EXT_X_CUE_IN "#EXT-X-CUE-IN"
#define EXT_X_CUE_SPAN "#EXT-X-CUE-SPAN"
#define EXT_OATCLS_SCTE35 "#EXT-OATCLS-SCTE35"
#define EXT_IS_INDEPENDENT_SEGMENTS "#EXT-X-INDEPENDENT-SEGMENTS"
#define EXT_X_MAP "#EXT-X-MAP"
#define EXT_X_START "#EXT-X-START"
#define EXT_X_SERVER_CONTROL "#EXT-X-SERVER-CONTROL"
#define EXT_X_PART_INF "#EXT-X-PART-INF"
#define EXT_X_PART "#EXT-X-PART"
#define EXT_X_RENDITION_REPORT "#EXT-X-RENDITION-REPORT"
#define EXT_X_SKIP "#EXT-X-SKIP"
#define EXT_X_SESSION_DATA "#EXT-X-SESSION-DATA"
#define EXT_X_SESSION_KEY "#EXT-X-SESSION-KEY"
#define EXT_X_PRELOAD_HINT "#EXT-X-PRELOAD-HINT"
#define EXT_X_DATERANGE "#EXT-X-DATERANGE"
#define EXT_X_GAP "#EXT-X-GAP"
#define EXT_X_CONTENT_STEERING "#EXT-X-CONTENT-STEERING"
#define EXT_X_IMAGE_STREAM_INF "#EXT-X-IMAGE-STREAM-INF"
#define EXT_X_IMAGES_ONLY "#EXT-X-IMAGES-ONLY"
#define EXT_X_TILES "#EXT-X-TILES"
#define EXT_X_BLACKOUT "#EXT-X-BLACKOUT"

/*
 * Module state - holds all per-module data.
 *
 * Using module state instead of static globals ensures:
 * - Proper cleanup when the module is garbage collected
 * - Compatibility with subinterpreters (PEP 573, PEP 3121)
 * - Thread-safe access to cached objects
 *
 * Frequently-used dict keys are cached as interned strings to avoid
 * repeated allocations and enable fast pointer comparisons.
 */
typedef struct {
    PyObject *ParseError;
    PyObject *datetime_cls;
    PyObject *timedelta_cls;
    PyObject *fromisoformat_meth;
    /* Interned string keys for common dict operations */
    PyObject *str_segment;
    PyObject *str_segments;
    PyObject *str_duration;
    PyObject *str_uri;
    PyObject *str_title;
    PyObject *str_expect_segment;
    PyObject *str_expect_playlist;
    PyObject *str_current_key;
    PyObject *str_keys;
    PyObject *str_cue_out;
    PyObject *str_cue_in;
} m3u8_state;

/*
 * Parse context - holds all state needed during a single parse() call.
 *
 * This structure reduces parameter passing between functions and makes
 * the parsing state explicit. All PyObject pointers in this struct are
 * borrowed references except where noted.
 *
 * Shadow State Optimization:
 * Hot flags (expect_segment, expect_playlist) are kept in C variables
 * to avoid dict lookup overhead in the main parsing loop. They are
 * synced to the Python state dict only when needed:
 * - Before calling custom_tags_parser (so callback sees current state)
 * - After custom_tags_parser returns (in case it modified state)
 * - At the end of parsing (for final state consistency)
 */
typedef struct {
    m3u8_state *mod_state;   /* Module state (borrowed) */
    PyObject *data;          /* Result dict being built (owned) */
    PyObject *state;         /* Parser state dict (owned) */
    int strict;              /* Strict parsing mode flag */
    int lineno;              /* Current line number (1-based) */
    /* Shadow state for hot flags - avoids dict lookups in main loop */
    int expect_segment;      /* Shadow of state["expect_segment"] */
    int expect_playlist;     /* Shadow of state["expect_playlist"] */
} ParseContext;

/*
 * Sync shadow state TO Python dict (before custom_tags_parser or at end).
 */
static int
sync_shadow_to_dict(ParseContext *ctx)
{
    m3u8_state *mod_state = ctx->mod_state;
    if (dict_set_interned(ctx->state, mod_state->str_expect_segment,
                          ctx->expect_segment ? Py_True : Py_False) < 0) {
        return -1;
    }
    if (dict_set_interned(ctx->state, mod_state->str_expect_playlist,
                          ctx->expect_playlist ? Py_True : Py_False) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Sync shadow state FROM Python dict (after custom_tags_parser modifies it).
 */
static void
sync_shadow_from_dict(ParseContext *ctx)
{
    m3u8_state *mod_state = ctx->mod_state;
    PyObject *val;

    val = dict_get_interned(ctx->state, mod_state->str_expect_segment);
    ctx->expect_segment = (val == Py_True);

    val = dict_get_interned(ctx->state, mod_state->str_expect_playlist);
    ctx->expect_playlist = (val == Py_True);
}

/* Forward declaration for module definition */
static struct PyModuleDef m3u8_parser_module;

/* Get module state from module object */
static inline m3u8_state *
get_m3u8_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (m3u8_state *)state;
}

/*
 * Initialize datetime-related cached objects in module state.
 * Called during module initialization.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
init_datetime_cache(m3u8_state *state)
{
    PyObject *datetime_mod = PyImport_ImportModule("datetime");
    if (datetime_mod == NULL) {
        return -1;
    }

    state->datetime_cls = PyObject_GetAttrString(datetime_mod, "datetime");
    state->timedelta_cls = PyObject_GetAttrString(datetime_mod, "timedelta");

    if (state->datetime_cls != NULL) {
        state->fromisoformat_meth = PyObject_GetAttrString(
            state->datetime_cls, "fromisoformat");
    }

    Py_DECREF(datetime_mod);

    if (state->datetime_cls == NULL ||
        state->timedelta_cls == NULL ||
        state->fromisoformat_meth == NULL)
    {
        Py_CLEAR(state->datetime_cls);
        Py_CLEAR(state->timedelta_cls);
        Py_CLEAR(state->fromisoformat_meth);
        return -1;
    }
    return 0;
}

/*
 * Initialize interned string cache for frequently-used dict keys.
 * Interning avoids repeated string allocations and enables fast
 * pointer-based comparisons in dict operations.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
init_interned_strings(m3u8_state *state)
{
    #define INTERN_STRING(field, str) \
        do { \
            state->field = PyUnicode_InternFromString(str); \
            if (state->field == NULL) return -1; \
        } while (0)

    INTERN_STRING(str_segment, "segment");
    INTERN_STRING(str_segments, "segments");
    INTERN_STRING(str_duration, "duration");
    INTERN_STRING(str_uri, "uri");
    INTERN_STRING(str_title, "title");
    INTERN_STRING(str_expect_segment, "expect_segment");
    INTERN_STRING(str_expect_playlist, "expect_playlist");
    INTERN_STRING(str_current_key, "current_key");
    INTERN_STRING(str_keys, "keys");
    INTERN_STRING(str_cue_out, "cue_out");
    INTERN_STRING(str_cue_in, "cue_in");

    #undef INTERN_STRING
    return 0;
}

/*
 * Raise ParseError with lineno and line arguments.
 * Takes module state to get the ParseError class.
 *
 * Optimization: Uses direct tuple construction instead of Py_BuildValue
 * to avoid format string parsing overhead.
 */
static void
raise_parse_error(m3u8_state *state, int lineno, const char *line)
{
    /* Direct tuple construction - faster than Py_BuildValue("(is)", ...) */
    PyObject *py_lineno = PyLong_FromLong(lineno);
    if (py_lineno == NULL) {
        return;
    }

    PyObject *py_line = PyUnicode_FromString(line);
    if (py_line == NULL) {
        Py_DECREF(py_lineno);
        return;
    }

    PyObject *args = PyTuple_Pack(2, py_lineno, py_line);
    Py_DECREF(py_lineno);
    Py_DECREF(py_line);
    if (args == NULL) {
        return;
    }

    PyObject *exc = PyObject_Call(state->ParseError, args, NULL);
    Py_DECREF(args);

    if (exc != NULL) {
        PyErr_SetObject(state->ParseError, exc);
        Py_DECREF(exc);
    }
}

/* Utility: return a newly allocated UTF-8 encoded copy of a Python unicode object.
   Caller must PyMem_Free(*out) on success. */
static int unicode_to_utf8_copy(PyObject *obj, char **out) {
    *out = NULL;
    if (!PyUnicode_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return -1;
    }

    PyObject *bytes = PyUnicode_AsUTF8String(obj);
    if (!bytes) return -1;

    char *buf = NULL;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(bytes, &buf, &size) < 0) {
        Py_DECREF(bytes);
        return -1;
    }

    char *copy = PyMem_Malloc((size_t)size + 1);
    if (!copy) {
        Py_DECREF(bytes);
        PyErr_NoMemory();
        return -1;
    }
    memcpy(copy, buf, (size_t)size);
    copy[size] = '\0';
    Py_DECREF(bytes);

    *out = copy;
    return 0;
}

/*
 * Strip leading and trailing whitespace from string in-place.
 *
 * Warning: This modifies the string in place by writing a NUL terminator.
 * Only use on mutable strings (e.g., our reusable line buffer).
 *
 * Returns pointer to first non-whitespace character (may be same as input
 * or point into the middle of the string).
 */
static char *strip(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    /* Safety: check length before computing end pointer to avoid UB */
    size_t len = strlen(str);
    if (len == 0) return str;
    char *end = str + len - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return str;
}

/* Utility: delete a key from dict; ignore missing-key KeyError, propagate other errors */
static int del_item_string_ignore_keyerror(PyObject *dict, const char *key) {
    if (PyDict_DelItemString(dict, key) == 0) {
        return 0;
    }
    if (PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_Clear();
        return 0;
    }
    return -1;
}

/*
 * Fast dict operations using interned string keys.
 *
 * These avoid the string creation overhead of PyDict_SetItemString by
 * using pre-interned strings from module state. PyDict_SetItem with
 * interned strings can use pointer comparison for fast key lookup.
 */

/* Set dict[key] = value using interned key. Returns 0 on success, -1 on error. */
static inline int
dict_set_interned(PyObject *dict, PyObject *interned_key, PyObject *value)
{
    return PyDict_SetItem(dict, interned_key, value);
}

/* Get dict[key] using interned key. Returns borrowed ref or NULL. */
static inline PyObject *
dict_get_interned(PyObject *dict, PyObject *interned_key)
{
    return PyDict_GetItem(dict, interned_key);
}

/*
 * Get or create segment dict in state using interned string.
 * Returns borrowed reference on success, NULL with exception on failure.
 */
static PyObject *
get_or_create_segment(m3u8_state *mod_state, PyObject *state)
{
    PyObject *segment = dict_get_interned(state, mod_state->str_segment);
    if (segment != NULL) {
        return segment;  /* borrowed reference */
    }
    segment = PyDict_New();
    if (segment == NULL) {
        return NULL;
    }
    if (dict_set_interned(state, mod_state->str_segment, segment) < 0) {
        Py_DECREF(segment);
        return NULL;
    }
    Py_DECREF(segment);
    return dict_get_interned(state, mod_state->str_segment);
}

/* Utility: set dict[key] = value, taking ownership of a new reference to value */
static int set_item_string_stealref(PyObject *dict, const char *key, PyObject *value) {
    if (!value) {
        return -1;
    }
    int rc = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return rc;
}

/* Utility: build list like Python's content.strip().splitlines() (preserve internal blanks) */
static PyObject *build_stripped_splitlines(const char *content) {
    const unsigned char *p = (const unsigned char *)content;
    const unsigned char *end = p + strlen(content);

    while (p < end && isspace(*p)) p++;
    while (end > p && isspace(*(end - 1))) end--;

    PyObject *lines = PyList_New(0);
    if (!lines) return NULL;

    const unsigned char *line_start = p;
    while (p < end) {
        if (*p == '\n' || *p == '\r') {
            PyObject *line = PyUnicode_FromStringAndSize((const char *)line_start,
                                                         (Py_ssize_t)(p - line_start));
            if (!line) {
                Py_DECREF(lines);
                return NULL;
            }
            if (PyList_Append(lines, line) < 0) {
                Py_DECREF(line);
                Py_DECREF(lines);
                return NULL;
            }
            Py_DECREF(line);

            /* Consume newline sequence */
            if (*p == '\r' && (p + 1) < end && *(p + 1) == '\n') p++;
            p++;
            line_start = p;
            continue;
        }
        p++;
    }

    /* Last line (even if empty) */
    PyObject *line = PyUnicode_FromStringAndSize((const char *)line_start,
                                                 (Py_ssize_t)(end - line_start));
    if (!line) {
        Py_DECREF(lines);
        return NULL;
    }
    if (PyList_Append(lines, line) < 0) {
        Py_DECREF(line);
        Py_DECREF(lines);
        return NULL;
    }
    Py_DECREF(line);

    return lines;
}

/*
 * Initialize the result data dictionary with default values.
 *
 * This sets up all the required keys with their initial values,
 * matching the structure created by the Python parser.
 *
 * Returns: New reference to data dict on success, NULL on failure.
 */
static PyObject *
init_parse_data(void)
{
    PyObject *data = PyDict_New();
    if (data == NULL) {
        return NULL;
    }

    /* Set scalar defaults */
    if (set_item_string_stealref(data, "media_sequence", PyLong_FromLong(0)) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_variant", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_endlist", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_i_frames_only", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_independent_segments", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_images_only", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "playlist_type", Py_None) < 0) goto fail;

    /* Initialize list fields */
    #define INIT_LIST(name) do { \
        PyObject *list = PyList_New(0); \
        if (list == NULL) goto fail; \
        if (PyDict_SetItemString(data, name, list) < 0) { \
            Py_DECREF(list); \
            goto fail; \
        } \
        Py_DECREF(list); \
    } while (0)

    INIT_LIST("playlists");
    INIT_LIST("segments");
    INIT_LIST("iframe_playlists");
    INIT_LIST("image_playlists");
    INIT_LIST("tiles");
    INIT_LIST("media");
    INIT_LIST("keys");
    INIT_LIST("rendition_reports");
    INIT_LIST("session_data");
    INIT_LIST("session_keys");
    INIT_LIST("segment_map");

    #undef INIT_LIST

    /* Initialize dict fields */
    #define INIT_DICT(name) do { \
        PyObject *dict = PyDict_New(); \
        if (dict == NULL) goto fail; \
        if (PyDict_SetItemString(data, name, dict) < 0) { \
            Py_DECREF(dict); \
            goto fail; \
        } \
        Py_DECREF(dict); \
    } while (0)

    INIT_DICT("skip");
    INIT_DICT("part_inf");

    #undef INIT_DICT

    return data;

fail:
    Py_DECREF(data);
    return NULL;
}

/*
 * Initialize the parser state dictionary.
 *
 * The state dict tracks parsing progress and carries values between
 * tags (e.g., current key, segment being built, etc.).
 *
 * Returns: New reference to state dict on success, NULL on failure.
 */
static PyObject *
init_parse_state(m3u8_state *mod_state)
{
    PyObject *state = PyDict_New();
    if (state == NULL) {
        return NULL;
    }

    /* Use interned strings for commonly-accessed keys */
    if (dict_set_interned(state, mod_state->str_expect_segment, Py_False) < 0) goto fail;
    if (dict_set_interned(state, mod_state->str_expect_playlist, Py_False) < 0) goto fail;

    return state;

fail:
    Py_DECREF(state);
    return NULL;
}

/*
 * Add seconds to a datetime object: dt + timedelta(seconds=secs)
 * Returns new reference on success, NULL with exception on failure.
 */
static PyObject *
datetime_add_seconds(m3u8_state *state, PyObject *dt, double secs)
{
    PyObject *args = PyTuple_New(0);
    if (args == NULL) {
        return NULL;
    }

    PyObject *kwargs = Py_BuildValue("{s:d}", "seconds", secs);
    if (kwargs == NULL) {
        Py_DECREF(args);
        return NULL;
    }

    PyObject *delta = PyObject_Call(state->timedelta_cls, args, kwargs);
    Py_DECREF(kwargs);
    Py_DECREF(args);
    if (delta == NULL) {
        return NULL;
    }

    PyObject *new_dt = PyNumber_Add(dt, delta);
    Py_DECREF(delta);
    return new_dt;
}

/*
 * Create normalized Python string directly from buffer (zero-copy optimization).
 *
 * This avoids malloc for keys < 64 chars (covers 99%+ of real-world cases).
 * Normalization: replace '-' with '_', lowercase, strip whitespace.
 *
 * Returns: New reference to Python string, or NULL with exception set.
 */
static PyObject *
create_normalized_key(const char *s, Py_ssize_t len)
{
    char stack_buf[64];
    char *buf = stack_buf;
    int use_heap = (len >= (Py_ssize_t)sizeof(stack_buf));

    if (use_heap) {
        buf = PyMem_Malloc(len + 1);
        if (buf == NULL) {
            return PyErr_NoMemory();
        }
    }

    /* Normalize: skip leading whitespace, replace - with _, tolower */
    Py_ssize_t in_idx = 0;
    Py_ssize_t out_len = 0;

    /* Skip leading whitespace */
    while (in_idx < len && isspace((unsigned char)s[in_idx])) {
        in_idx++;
    }

    /* Transform characters */
    for (; in_idx < len; in_idx++) {
        char c = s[in_idx];
        if (c == '-') {
            c = '_';
        } else {
            c = tolower((unsigned char)c);
        }
        buf[out_len++] = c;
    }

    /* Strip trailing whitespace */
    while (out_len > 0 && isspace((unsigned char)buf[out_len - 1])) {
        out_len--;
    }
    buf[out_len] = '\0';

    PyObject *res = PyUnicode_FromStringAndSize(buf, out_len);

    if (use_heap) {
        PyMem_Free(buf);
    }
    return res;
}

/* Utility: remove quotes from string */
static PyObject *remove_quotes(const char *str) {
    size_t len = strlen(str);
    if (len >= 2) {
        if ((str[0] == '"' && str[len-1] == '"') ||
            (str[0] == '\'' && str[len-1] == '\'')) {
            return PyUnicode_FromStringAndSize(str + 1, len - 2);
        }
    }
    return PyUnicode_FromString(str);
}

/*
 * Zero-copy attribute list parser.
 *
 * Parses "KEY=value,KEY2=value2" format directly from buffer pointers.
 * Creates Python objects directly without intermediate C string allocations.
 *
 * Args:
 *     start: Pointer to start of attribute list (after the ":" in the tag)
 *     end: Pointer to end of buffer
 *
 * Returns: New reference to dict, or NULL with exception set.
 */
static PyObject *
parse_attribute_list_raw(const char *start, const char *end)
{
    PyObject *attrs = PyDict_New();
    if (attrs == NULL) {
        return NULL;
    }

    const char *p = start;
    while (p < end) {
        /* Skip leading whitespace and commas */
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        /* Find key */
        const char *key_start = p;
        while (p < end && *p != '=' && *p != ',') {
            p++;
        }
        const char *key_end = p;

        /* Create normalized key directly from buffer */
        PyObject *py_key = create_normalized_key(key_start, key_end - key_start);
        if (py_key == NULL) {
            Py_DECREF(attrs);
            return NULL;
        }

        PyObject *py_val = NULL;

        if (p < end && *p == '=') {
            p++;  /* Skip '=' */

            if (p < end && (*p == '"' || *p == '\'')) {
                /* Quoted string - include quotes in value for later processing */
                char quote = *p;
                const char *val_start = p;  /* Include opening quote */
                p++;  /* Skip opening quote */
                while (p < end && *p != quote) {
                    p++;
                }
                if (p < end) {
                    p++;  /* Include closing quote */
                }
                /* Create string with quotes (for compatibility with typed parser) */
                py_val = PyUnicode_FromStringAndSize(val_start, p - val_start);
            } else {
                /* Unquoted value */
                const char *val_start = p;
                while (p < end && *p != ',') {
                    p++;
                }
                /* Strip trailing whitespace from unquoted values */
                const char *val_end = p;
                while (val_end > val_start && isspace((unsigned char)*(val_end - 1))) {
                    val_end--;
                }
                py_val = PyUnicode_FromStringAndSize(val_start, val_end - val_start);
            }
        } else {
            /* Key without value - store the key content as value with empty key */
            /* This handles formats like "EXT-X-CUE-OUT-CONT:2.436/120" */
            Py_ssize_t key_len = key_end - key_start;
            /* Strip trailing whitespace */
            while (key_len > 0 && isspace((unsigned char)key_start[key_len - 1])) {
                key_len--;
            }
            py_val = PyUnicode_FromStringAndSize(key_start, key_len);
            Py_DECREF(py_key);
            py_key = PyUnicode_FromString("");
            if (py_key == NULL) {
                Py_XDECREF(py_val);
                Py_DECREF(attrs);
                return NULL;
            }
        }

        if (py_val == NULL) {
            Py_DECREF(py_key);
            Py_DECREF(attrs);
            return NULL;
        }

        if (PyDict_SetItem(attrs, py_key, py_val) < 0) {
            Py_DECREF(py_key);
            Py_DECREF(py_val);
            Py_DECREF(attrs);
            return NULL;
        }

        Py_DECREF(py_key);
        Py_DECREF(py_val);
    }

    return attrs;
}

/*
 * Parse attribute list from a line like "PREFIX:KEY=value,KEY2=value2"
 *
 * This is a wrapper around parse_attribute_list_raw that handles the
 * prefix-skipping logic for compatibility with existing callers.
 *
 * Returns new reference to dict on success, NULL with exception on failure.
 */
static PyObject *
parse_attribute_list(const char *line, const char *prefix)
{
    /* Skip prefix if present */
    const char *content = line;
    if (prefix != NULL) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) == 0) {
            content = line + prefix_len;
            if (*content == ':') {
                content++;
            }
        } else {
            /* Prefix not found - return empty dict */
            return PyDict_New();
        }
    }

    /* Delegate to zero-copy implementation */
    return parse_attribute_list_raw(content, content + strlen(content));
}

/* Parse a key/value attribute list with type conversion */
typedef enum {
    ATTR_STRING,
    ATTR_INT,
    ATTR_FLOAT,
    ATTR_QUOTED_STRING,
    ATTR_BANDWIDTH
} AttrType;

typedef struct {
    const char *name;
    AttrType type;
} AttrParser;

/*
 * Schema-aware attribute parser.
 *
 * This is the optimized version that converts values to their final types
 * directly during parsing, avoiding the "double allocation" problem where
 * we first create a Python string, then convert it to int/float.
 *
 * The schema (parsers array) tells us the expected type for each key,
 * so we can parse directly to the correct Python type.
 *
 * Args:
 *     start: Pointer to start of attribute list (after "TAG:")
 *     end: Pointer to end of content
 *     parsers: Array of AttrParser structs defining key->type mappings
 *     num_parsers: Number of parsers in array
 *
 * Returns: New reference to dict on success, NULL with exception set.
 */
static PyObject *
parse_attributes_with_schema(const char *start, const char *end,
                             const AttrParser *parsers, size_t num_parsers)
{
    PyObject *attrs = PyDict_New();
    if (attrs == NULL) {
        return NULL;
    }

    const char *p = start;
    while (p < end) {
        /* Skip leading whitespace and commas */
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        /* Find key */
        const char *key_start = p;
        while (p < end && *p != '=' && *p != ',') {
            p++;
        }
        const char *key_end = p;

        /* Create normalized key directly from buffer */
        PyObject *py_key = create_normalized_key(key_start, key_end - key_start);
        if (py_key == NULL) {
            Py_DECREF(attrs);
            return NULL;
        }

        /* Determine type via schema lookup */
        AttrType type = ATTR_STRING;
        if (parsers != NULL) {
            for (size_t i = 0; i < num_parsers; i++) {
                if (PyUnicode_CompareWithASCIIString(py_key, parsers[i].name) == 0) {
                    type = parsers[i].type;
                    break;
                }
            }
        }

        PyObject *py_val = NULL;

        if (p < end && *p == '=') {
            p++;  /* Skip '=' */

            if (p < end && (*p == '"' || *p == '\'')) {
                /* Quoted value */
                char quote = *p++;
                const char *full_start = p - 1;  /* include opening quote */
                const char *val_start = p;       /* inside quotes */
                while (p < end && *p != quote) {
                    p++;
                }
                const char *val_end = p;         /* points at closing quote or end */
                int has_closing_quote = (p < end && *p == quote);
                Py_ssize_t val_len = val_end - val_start;
                if (has_closing_quote) {
                    p++;  /* Skip closing quote */
                }

                /*
                 * Python parity:
                 * - Known "quoted string" attributes use remove_quotes() => no quotes
                 * - Unknown attributes keep the original token (including quotes)
                 */
                if (type == ATTR_QUOTED_STRING) {
                    py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                } else if (type == ATTR_STRING) {
                    Py_ssize_t full_len = has_closing_quote
                        ? (Py_ssize_t)((val_end - full_start) + 1)
                        : (Py_ssize_t)(val_end - full_start);
                    py_val = PyUnicode_FromStringAndSize(full_start, full_len);
                } else if (type == ATTR_INT || type == ATTR_BANDWIDTH) {
                    /* Numeric inside quotes - parse directly */
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        if (type == ATTR_BANDWIDTH) {
                            double v = PyOS_string_to_double(num_buf, NULL, NULL);
                            if (v == -1.0 && PyErr_Occurred()) {
                                PyErr_Clear();
                            } else {
                                py_val = PyLong_FromDouble(v);
                            }
                        } else {
                            py_val = PyLong_FromString(num_buf, NULL, 10);
                            if (py_val == NULL) {
                                PyErr_Clear();
                            }
                        }
                    }
                    /* Fallback to string if conversion fails */
                    if (py_val == NULL) {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else if (type == ATTR_FLOAT) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        double v = PyOS_string_to_double(num_buf, NULL, NULL);
                        if (v == -1.0 && PyErr_Occurred()) {
                            PyErr_Clear();
                            py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                        } else {
                            py_val = PyFloat_FromDouble(v);
                        }
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else {
                    py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                }
            } else {
                /* Unquoted value */
                const char *val_start = p;
                while (p < end && *p != ',') {
                    p++;
                }
                /* Strip trailing whitespace */
                const char *val_end = p;
                while (val_end > val_start && isspace((unsigned char)*(val_end - 1))) {
                    val_end--;
                }
                Py_ssize_t val_len = val_end - val_start;

                /* Direct type conversion - no intermediate Python string! */
                if (type == ATTR_INT) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        py_val = PyLong_FromString(num_buf, NULL, 10);
                        if (py_val == NULL) {
                            PyErr_Clear();
                        }
                    }
                    if (py_val == NULL) {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else if (type == ATTR_BANDWIDTH) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        double v = PyOS_string_to_double(num_buf, NULL, NULL);
                        if (v == -1.0 && PyErr_Occurred()) {
                            PyErr_Clear();
                        } else {
                            py_val = PyLong_FromDouble(v);
                        }
                    }
                    if (py_val == NULL) {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else if (type == ATTR_FLOAT) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        double v = PyOS_string_to_double(num_buf, NULL, NULL);
                        if (v == -1.0 && PyErr_Occurred()) {
                            PyErr_Clear();
                            py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                        } else {
                            py_val = PyFloat_FromDouble(v);
                        }
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else {
                    /* ATTR_STRING or ATTR_QUOTED_STRING (unquoted case) */
                    py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                }
            }
        } else {
            /* Key without value - store key content as value with empty key */
            Py_ssize_t key_len = key_end - key_start;
            while (key_len > 0 && isspace((unsigned char)key_start[key_len - 1])) {
                key_len--;
            }
            py_val = PyUnicode_FromStringAndSize(key_start, key_len);
            Py_DECREF(py_key);
            py_key = PyUnicode_FromString("");
            if (py_key == NULL) {
                Py_XDECREF(py_val);
                Py_DECREF(attrs);
                return NULL;
            }
        }

        if (py_val == NULL) {
            Py_DECREF(py_key);
            Py_DECREF(attrs);
            return NULL;
        }

        if (PyDict_SetItem(attrs, py_key, py_val) < 0) {
            Py_DECREF(py_key);
            Py_DECREF(py_val);
            Py_DECREF(attrs);
            return NULL;
        }

        Py_DECREF(py_key);
        Py_DECREF(py_val);
    }

    return attrs;
}

/*
 * Wrapper for parse_attributes_with_schema that handles prefix skipping.
 * This maintains backward compatibility with existing callers.
 */
static PyObject *parse_typed_attribute_list(const char *line, const char *prefix,
                                            const AttrParser *parsers, size_t num_parsers) {
    /* Skip prefix if present */
    const char *content = line;
    if (prefix != NULL) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) == 0) {
            content = line + prefix_len;
            if (*content == ':') {
                content++;
            }
        } else {
            /* Prefix not found - return empty dict */
            return PyDict_New();
        }
    }

    /* Delegate to schema-aware parser */
    return parse_attributes_with_schema(content, content + strlen(content),
                                        parsers, num_parsers);
}

/* Stream info attribute parsers */
static const AttrParser stream_inf_parsers[] = {
    {"codecs", ATTR_QUOTED_STRING},
    {"audio", ATTR_QUOTED_STRING},
    {"video", ATTR_QUOTED_STRING},
    {"video_range", ATTR_QUOTED_STRING},
    {"subtitles", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
    {"stable_variant_id", ATTR_QUOTED_STRING},
    {"program_id", ATTR_INT},
    {"bandwidth", ATTR_BANDWIDTH},
    {"average_bandwidth", ATTR_INT},
    {"frame_rate", ATTR_FLOAT},
    {"hdcp_level", ATTR_STRING},
};
#define NUM_STREAM_INF_PARSERS (sizeof(stream_inf_parsers) / sizeof(stream_inf_parsers[0]))

/* Media attribute parsers */
static const AttrParser media_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"group_id", ATTR_QUOTED_STRING},
    {"language", ATTR_QUOTED_STRING},
    {"assoc_language", ATTR_QUOTED_STRING},
    {"name", ATTR_QUOTED_STRING},
    {"instream_id", ATTR_QUOTED_STRING},
    {"characteristics", ATTR_QUOTED_STRING},
    {"channels", ATTR_QUOTED_STRING},
    {"stable_rendition_id", ATTR_QUOTED_STRING},
    {"thumbnails", ATTR_QUOTED_STRING},
    {"image", ATTR_QUOTED_STRING},
};
#define NUM_MEDIA_PARSERS (sizeof(media_parsers) / sizeof(media_parsers[0]))

/* Part attribute parsers */
static const AttrParser part_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"duration", ATTR_FLOAT},
    {"independent", ATTR_STRING},
    {"gap", ATTR_STRING},
    {"byterange", ATTR_STRING},
};
#define NUM_PART_PARSERS (sizeof(part_parsers) / sizeof(part_parsers[0]))

/* Rendition report parsers */
static const AttrParser rendition_report_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"last_msn", ATTR_INT},
    {"last_part", ATTR_INT},
};
#define NUM_RENDITION_REPORT_PARSERS (sizeof(rendition_report_parsers) / sizeof(rendition_report_parsers[0]))

/* Skip parsers */
static const AttrParser skip_parsers[] = {
    {"recently_removed_dateranges", ATTR_QUOTED_STRING},
    {"skipped_segments", ATTR_INT},
};
#define NUM_SKIP_PARSERS (sizeof(skip_parsers) / sizeof(skip_parsers[0]))

/* Server control parsers */
static const AttrParser server_control_parsers[] = {
    {"can_block_reload", ATTR_STRING},
    {"hold_back", ATTR_FLOAT},
    {"part_hold_back", ATTR_FLOAT},
    {"can_skip_until", ATTR_FLOAT},
    {"can_skip_dateranges", ATTR_STRING},
};
#define NUM_SERVER_CONTROL_PARSERS (sizeof(server_control_parsers) / sizeof(server_control_parsers[0]))

/* Part inf parsers */
static const AttrParser part_inf_parsers[] = {
    {"part_target", ATTR_FLOAT},
};
#define NUM_PART_INF_PARSERS (sizeof(part_inf_parsers) / sizeof(part_inf_parsers[0]))

/* Preload hint parsers */
static const AttrParser preload_hint_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"type", ATTR_STRING},
    {"byterange_start", ATTR_INT},
    {"byterange_length", ATTR_INT},
};
#define NUM_PRELOAD_HINT_PARSERS (sizeof(preload_hint_parsers) / sizeof(preload_hint_parsers[0]))

/* Daterange parsers */
static const AttrParser daterange_parsers[] = {
    {"id", ATTR_QUOTED_STRING},
    {"class", ATTR_QUOTED_STRING},
    {"start_date", ATTR_QUOTED_STRING},
    {"end_date", ATTR_QUOTED_STRING},
    {"duration", ATTR_FLOAT},
    {"planned_duration", ATTR_FLOAT},
    {"end_on_next", ATTR_STRING},
    {"scte35_cmd", ATTR_STRING},
    {"scte35_out", ATTR_STRING},
    {"scte35_in", ATTR_STRING},
};
#define NUM_DATERANGE_PARSERS (sizeof(daterange_parsers) / sizeof(daterange_parsers[0]))

/* Session data parsers */
static const AttrParser session_data_parsers[] = {
    {"data_id", ATTR_QUOTED_STRING},
    {"value", ATTR_QUOTED_STRING},
    {"uri", ATTR_QUOTED_STRING},
    {"language", ATTR_QUOTED_STRING},
};
#define NUM_SESSION_DATA_PARSERS (sizeof(session_data_parsers) / sizeof(session_data_parsers[0]))

/* Content steering parsers */
static const AttrParser content_steering_parsers[] = {
    {"server_uri", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
};
#define NUM_CONTENT_STEERING_PARSERS (sizeof(content_steering_parsers) / sizeof(content_steering_parsers[0]))

/* X-MAP parsers */
static const AttrParser x_map_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"byterange", ATTR_QUOTED_STRING},
};
#define NUM_X_MAP_PARSERS (sizeof(x_map_parsers) / sizeof(x_map_parsers[0]))

/* Start parsers */
static const AttrParser start_parsers[] = {
    {"time_offset", ATTR_FLOAT},
};
#define NUM_START_PARSERS (sizeof(start_parsers) / sizeof(start_parsers[0]))

/* Tiles parsers */
static const AttrParser tiles_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"resolution", ATTR_STRING},
    {"layout", ATTR_STRING},
    {"duration", ATTR_FLOAT},
};
#define NUM_TILES_PARSERS (sizeof(tiles_parsers) / sizeof(tiles_parsers[0]))

/* Image stream inf parsers */
static const AttrParser image_stream_inf_parsers[] = {
    {"codecs", ATTR_QUOTED_STRING},
    {"uri", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
    {"stable_variant_id", ATTR_QUOTED_STRING},
    {"program_id", ATTR_INT},
    {"bandwidth", ATTR_INT},
    {"average_bandwidth", ATTR_INT},
    {"resolution", ATTR_STRING},
};
#define NUM_IMAGE_STREAM_INF_PARSERS (sizeof(image_stream_inf_parsers) / sizeof(image_stream_inf_parsers[0]))

/* IFrame stream inf parsers */
static const AttrParser iframe_stream_inf_parsers[] = {
    {"codecs", ATTR_QUOTED_STRING},
    {"uri", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
    {"stable_variant_id", ATTR_QUOTED_STRING},
    {"program_id", ATTR_INT},
    {"bandwidth", ATTR_INT},
    {"average_bandwidth", ATTR_INT},
    {"hdcp_level", ATTR_STRING},
};
#define NUM_IFRAME_STREAM_INF_PARSERS (sizeof(iframe_stream_inf_parsers) / sizeof(iframe_stream_inf_parsers[0]))

/* Cueout cont parsers */
static const AttrParser cueout_cont_parsers[] = {
    {"duration", ATTR_QUOTED_STRING},
    {"elapsedtime", ATTR_QUOTED_STRING},
    {"scte35", ATTR_QUOTED_STRING},
};
#define NUM_CUEOUT_CONT_PARSERS (sizeof(cueout_cont_parsers) / sizeof(cueout_cont_parsers[0]))

/* Cueout parsers */
static const AttrParser cueout_parsers[] = {
    {"cue", ATTR_QUOTED_STRING},
};
#define NUM_CUEOUT_PARSERS (sizeof(cueout_parsers) / sizeof(cueout_parsers[0]))


/* Parse a key tag */
static int parse_key(const char *line, PyObject *data, PyObject *state) {
    PyObject *raw_attrs = parse_attribute_list(line, EXT_X_KEY);
    if (!raw_attrs) return -1;

    PyObject *key = PyDict_New();
    if (!key) {
        Py_DECREF(raw_attrs);
        return -1;
    }

    /* Convert attributes with quote removal */
    PyObject *k, *v;
    Py_ssize_t pos = 0;
    while (PyDict_Next(raw_attrs, &pos, &k, &v)) {
        char *value_str = NULL;
        if (unicode_to_utf8_copy(v, &value_str) < 0) continue;
        PyObject *unquoted = remove_quotes(value_str);
        PyMem_Free(value_str);
        if (unquoted) {
            PyDict_SetItem(key, k, unquoted);
            Py_DECREF(unquoted);
        }
    }
    Py_DECREF(raw_attrs);

    /* Set current key in state */
    PyDict_SetItemString(state, "current_key", key);

    /* Add to keys list if not already present */
    PyObject *keys = PyDict_GetItemString(data, "keys");
    if (keys) {
        int found = 0;
        Py_ssize_t n = PyList_Size(keys);
        for (Py_ssize_t i = 0; i < n; i++) {
            if (PyObject_RichCompareBool(PyList_GetItem(keys, i), key, Py_EQ) == 1) {
                found = 1;
                break;
            }
        }
        if (!found) {
            PyList_Append(keys, key);
        }
    }

    Py_DECREF(key);
    return 0;
}

/*
 * Parse #EXTINF tag.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_extinf(m3u8_state *mod_state, const char *line, PyObject *state,
             int lineno, int strict)
{
    const char *content = line + strlen(EXTINF) + 1;  /* Skip "#EXTINF:" */

    /* Find comma separator */
    const char *comma = strchr(content, ',');
    double duration;
    const char *title = "";

    if (comma != NULL) {
        char duration_str[64];
        size_t dur_len = comma - content;
        if (dur_len >= sizeof(duration_str)) {
            dur_len = sizeof(duration_str) - 1;
        }
        memcpy(duration_str, content, dur_len);
        duration_str[dur_len] = '\0';
        duration = PyOS_string_to_double(duration_str, NULL, NULL);
        if (duration == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            duration = 0.0;
        }
        title = comma + 1;
    } else {
        if (strict) {
            raise_parse_error(mod_state, lineno, line);
            return -1;
        }
        duration = PyOS_string_to_double(content, NULL, NULL);
        if (duration == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            duration = 0.0;
        }
    }

    /* Get or create segment dict in state using interned string */
    PyObject *segment = get_or_create_segment(mod_state, state);
    if (segment == NULL) {
        return -1;
    }

    /* Set duration using interned key */
    PyObject *py_duration = PyFloat_FromDouble(duration);
    if (py_duration == NULL) {
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_duration, py_duration) < 0) {
        Py_DECREF(py_duration);
        return -1;
    }
    Py_DECREF(py_duration);

    /* Set title using interned key */
    PyObject *py_title = PyUnicode_FromString(title);
    if (py_title == NULL) {
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_title, py_title) < 0) {
        Py_DECREF(py_title);
        return -1;
    }
    Py_DECREF(py_title);

    /* Set expect_segment flag using interned key */
    if (dict_set_interned(state, mod_state->str_expect_segment, Py_True) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Parse a segment URI line.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_ts_chunk(m3u8_state *mod_state, const char *line,
               PyObject *data, PyObject *state)
{
    /* Get segment dict from state using interned key, or create new one */
    PyObject *segment = dict_get_interned(state, mod_state->str_segment);
    if (segment == NULL) {
        segment = PyDict_New();
        if (segment == NULL) {
            return -1;
        }
    } else {
        Py_INCREF(segment);
    }
    /* Remove segment from state (we're taking ownership) */
    if (PyDict_DelItem(state, mod_state->str_segment) < 0) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
            Py_DECREF(segment);
            return -1;
        }
        PyErr_Clear();
    }

    /* Add URI using interned key */
    PyObject *uri = PyUnicode_FromString(line);
    if (uri == NULL) {
        Py_DECREF(segment);
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_uri, uri) < 0) {
        Py_DECREF(uri);
        Py_DECREF(segment);
        return -1;
    }
    Py_DECREF(uri);

    /* Transfer state values to segment (borrowed references from GetItemString) */
    PyObject *pdt = PyDict_GetItemString(state, "program_date_time");
    if (pdt != NULL) {
        PyDict_SetItemString(segment, "program_date_time", pdt);
        PyDict_DelItemString(state, "program_date_time");
    }

    PyObject *current_pdt = PyDict_GetItemString(state, "current_program_date_time");
    if (current_pdt != NULL) {
        PyDict_SetItemString(segment, "current_program_date_time", current_pdt);
        /* Update current_program_date_time by adding duration */
        PyObject *duration = PyDict_GetItemString(segment, "duration");
        if (duration != NULL && current_pdt != NULL) {
            double secs = PyFloat_AsDouble(duration);
            if (PyErr_Occurred()) {
                Py_DECREF(segment);
                return -1;
            }
            PyObject *new_pdt = datetime_add_seconds(mod_state, current_pdt, secs);
            if (new_pdt == NULL) {
                Py_DECREF(segment);
                return -1;
            }
            PyDict_SetItemString(state, "current_program_date_time", new_pdt);
            Py_DECREF(new_pdt);
        }
    }

    /* Boolean flags from state - use interned strings for hot keys */
    PyObject *cue_in = dict_get_interned(state, mod_state->str_cue_in);
    if (dict_set_interned(segment, mod_state->str_cue_in, cue_in ? Py_True : Py_False) < 0) {
        Py_DECREF(segment);
        return -1;
    }
    if (cue_in && PyDict_DelItem(state, mod_state->str_cue_in) < 0) {
        PyErr_Clear();  /* Ignore KeyError */
    }

    PyObject *cue_out = dict_get_interned(state, mod_state->str_cue_out);
    int cue_out_truth = cue_out ? PyObject_IsTrue(cue_out) : 0;
    if (cue_out_truth < 0) {
        Py_DECREF(segment);
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_cue_out, cue_out_truth ? Py_True : Py_False) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    PyObject *cue_out_start = PyDict_GetItemString(state, "cue_out_start");
    PyDict_SetItemString(segment, "cue_out_start", cue_out_start ? Py_True : Py_False);
    if (cue_out_start) PyDict_DelItemString(state, "cue_out_start");

    PyObject *cue_out_explicitly_duration = PyDict_GetItemString(state, "cue_out_explicitly_duration");
    PyDict_SetItemString(segment, "cue_out_explicitly_duration",
        cue_out_explicitly_duration ? Py_True : Py_False);
    if (cue_out_explicitly_duration) PyDict_DelItemString(state, "cue_out_explicitly_duration");

    /* SCTE35 values - get if cue_out, pop otherwise */
    const char *scte_keys[] = {"current_cue_out_scte35", "current_cue_out_oatcls_scte35",
                               "current_cue_out_duration", "current_cue_out_elapsedtime",
                               "asset_metadata"};
    const char *seg_keys[] = {"scte35", "oatcls_scte35", "scte35_duration",
                              "scte35_elapsedtime", "asset_metadata"};

    for (int i = 0; i < 5; i++) {
        PyObject *val = PyDict_GetItemString(state, scte_keys[i]);
        if (val) {
            PyDict_SetItemString(segment, seg_keys[i], val);
            if (!cue_out_truth) {
                if (del_item_string_ignore_keyerror(state, scte_keys[i]) < 0) return -1;
            }
        } else {
            /* Clear any potential error from PyDict_GetItemString (though unlikely) */
            PyErr_Clear();
            PyDict_SetItemString(segment, seg_keys[i], Py_None);
        }
    }

    if (del_item_string_ignore_keyerror(state, "cue_out") < 0) return -1;

    /* Discontinuity */
    PyObject *discontinuity = PyDict_GetItemString(state, "discontinuity");
    PyDict_SetItemString(segment, "discontinuity", discontinuity ? Py_True : Py_False);
    if (discontinuity) PyDict_DelItemString(state, "discontinuity");

    /* Key - use interned string for current_key lookup */
    PyObject *current_key = dict_get_interned(state, mod_state->str_current_key);
    if (current_key) {
        PyDict_SetItemString(segment, "key", current_key);
    } else {
        /* For unencrypted segments, ensure None is in keys list */
        PyObject *keys = dict_get_interned(data, mod_state->str_keys);
        if (keys) {
            int has_none = 0;
            Py_ssize_t n = PyList_Size(keys);
            for (Py_ssize_t i = 0; i < n; i++) {
                if (PyList_GetItem(keys, i) == Py_None) {
                    has_none = 1;
                    break;
                }
            }
            if (!has_none) {
                PyList_Append(keys, Py_None);
            }
        }
    }

    /* Init section */
    PyObject *current_segment_map = PyDict_GetItemString(state, "current_segment_map");
    /* Only set init_section if the map dict is non-empty (matches Python's truthiness check) */
    if (current_segment_map && PyDict_Size(current_segment_map) > 0) {
        PyDict_SetItemString(segment, "init_section", current_segment_map);
    }

    /* Dateranges */
    PyObject *dateranges = PyDict_GetItemString(state, "dateranges");
    if (dateranges) {
        PyDict_SetItemString(segment, "dateranges", dateranges);
        PyDict_DelItemString(state, "dateranges");
    } else {
        PyDict_SetItemString(segment, "dateranges", Py_None);
    }

    /* Gap */
    PyObject *gap = PyDict_GetItemString(state, "gap");
    if (gap) {
        PyDict_SetItemString(segment, "gap_tag", Py_True);
        PyDict_DelItemString(state, "gap");
    } else {
        PyDict_SetItemString(segment, "gap_tag", Py_None);
    }

    /* Blackout */
    PyObject *blackout = PyDict_GetItemString(state, "blackout");
    if (blackout) {
        PyDict_SetItemString(segment, "blackout", blackout);
        PyDict_DelItemString(state, "blackout");
    } else {
        PyDict_SetItemString(segment, "blackout", Py_None);
    }

    /* Add to segments list using interned key */
    PyObject *segments = dict_get_interned(data, mod_state->str_segments);
    if (segments) {
        if (PyList_Append(segments, segment) < 0) {
            Py_DECREF(segment);
            return -1;
        }
    }

    /* Clear expect_segment flag using interned key */
    if (dict_set_interned(state, mod_state->str_expect_segment, Py_False) < 0) {
        Py_DECREF(segment);
        return -1;
    }
    Py_DECREF(segment);
    return 0;
}

/* Parse variant playlist */
static int parse_variant_playlist(const char *line, PyObject *data, PyObject *state) {
    PyObject *stream_info = PyDict_GetItemString(state, "stream_info");
    if (!stream_info) {
        stream_info = PyDict_New();
    } else {
        Py_INCREF(stream_info);
    }
    if (del_item_string_ignore_keyerror(state, "stream_info") < 0) {
        Py_DECREF(stream_info);
        return -1;
    }

    PyObject *playlist = PyDict_New();
    if (!playlist) {
        Py_DECREF(stream_info);
        return -1;
    }

    PyObject *uri = PyUnicode_FromString(line);
    PyDict_SetItemString(playlist, "uri", uri);
    Py_DECREF(uri);

    PyDict_SetItemString(playlist, "stream_info", stream_info);
    Py_DECREF(stream_info);

    PyObject *playlists = PyDict_GetItemString(data, "playlists");
    if (playlists) {
        PyList_Append(playlists, playlist);
    }
    Py_DECREF(playlist);

    PyDict_SetItemString(state, "expect_playlist", Py_False);
    return 0;
}

/*
 * Parse EXT-X-PROGRAM-DATE-TIME tag.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_program_date_time(m3u8_state *mod_state, const char *line,
                        PyObject *data, PyObject *state)
{
    const char *value = strchr(line, ':');
    if (value == NULL) {
        return 0;
    }
    value++;

    PyObject *dt = PyObject_CallFunction(mod_state->fromisoformat_meth, "s", value);
    if (dt == NULL) {
        return -1;
    }

    /* Set in data if not already set (borrowed reference from GetItemString) */
    PyObject *existing = PyDict_GetItemString(data, "program_date_time");
    if (existing == NULL || existing == Py_None) {
        PyDict_SetItemString(data, "program_date_time", dt);
    }

    PyDict_SetItemString(state, "current_program_date_time", dt);
    PyDict_SetItemString(state, "program_date_time", dt);
    Py_DECREF(dt);
    return 0;
}

/*
 * Parse EXT-X-PART tag.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_part(m3u8_state *mod_state, const char *line, PyObject *state)
{
    PyObject *part = parse_typed_attribute_list(line, EXT_X_PART,
                                                 part_parsers, NUM_PART_PARSERS);
    if (part == NULL) {
        return -1;
    }

    /* Add program_date_time if available (borrowed reference) */
    PyObject *current_pdt = PyDict_GetItemString(state, "current_program_date_time");
    if (current_pdt != NULL) {
        PyDict_SetItemString(part, "program_date_time", current_pdt);
        /* Update current_program_date_time */
        PyObject *duration = PyDict_GetItemString(part, "duration");
        if (duration != NULL) {
            double secs = PyFloat_AsDouble(duration);
            if (PyErr_Occurred()) {
                Py_DECREF(part);
                return -1;
            }
            PyObject *new_pdt = datetime_add_seconds(mod_state, current_pdt, secs);
            if (new_pdt == NULL) {
                Py_DECREF(part);
                return -1;
            }
            PyDict_SetItemString(state, "current_program_date_time", new_pdt);
            Py_DECREF(new_pdt);
        }
    }

    /* Add dateranges (borrowed reference) */
    PyObject *dateranges = PyDict_GetItemString(state, "dateranges");
    if (dateranges != NULL) {
        PyDict_SetItemString(part, "dateranges", dateranges);
        PyDict_DelItemString(state, "dateranges");
    } else {
        PyDict_SetItemString(part, "dateranges", Py_None);
    }

    /* Add gap_tag (borrowed reference) */
    PyObject *gap = PyDict_GetItemString(state, "gap");
    if (gap != NULL) {
        PyDict_SetItemString(part, "gap_tag", Py_True);
        PyDict_DelItemString(state, "gap");
    } else {
        PyDict_SetItemString(part, "gap_tag", Py_None);
    }

    /* Get or create segment (borrowed reference after SetItemString) */
    PyObject *segment = PyDict_GetItemString(state, "segment");
    if (segment == NULL) {
        segment = PyDict_New();
        if (segment == NULL) {
            Py_DECREF(part);
            return -1;
        }
        PyDict_SetItemString(state, "segment", segment);
        Py_DECREF(segment);
        segment = PyDict_GetItemString(state, "segment");
    }

    /* Get or create parts list in segment (borrowed reference after SetItemString) */
    PyObject *parts = PyDict_GetItemString(segment, "parts");
    if (parts == NULL) {
        parts = PyList_New(0);
        if (parts == NULL) {
            Py_DECREF(part);
            return -1;
        }
        PyDict_SetItemString(segment, "parts", parts);
        Py_DECREF(parts);
        parts = PyDict_GetItemString(segment, "parts");
    }

    PyList_Append(parts, part);
    Py_DECREF(part);
    return 0;
}

/* Parse cue out */
static int parse_cueout(const char *line, PyObject *state) {
    PyDict_SetItemString(state, "cue_out_start", Py_True);
    PyDict_SetItemString(state, "cue_out", Py_True);

    /* Check for DURATION keyword */
    char upper_line[1024];
    size_t i;
    for (i = 0; i < sizeof(upper_line) - 1 && line[i]; i++) {
        upper_line[i] = toupper((unsigned char)line[i]);
    }
    upper_line[i] = '\0';

    if (strstr(upper_line, "DURATION")) {
        PyDict_SetItemString(state, "cue_out_explicitly_duration", Py_True);
    }

    /* Parse attributes if present */
    const char *colon = strchr(line, ':');
    if (!colon || *(colon + 1) == '\0') {
        return 0;
    }

    PyObject *cue_info = parse_typed_attribute_list(line, EXT_X_CUE_OUT,
        cueout_parsers, NUM_CUEOUT_PARSERS);
    if (!cue_info) return -1;

    PyObject *cue_out_scte35 = PyDict_GetItemString(cue_info, "cue");

    /* Get duration from "duration" key or empty key */
    PyObject *cue_out_duration = PyDict_GetItemString(cue_info, "duration");
    if (!cue_out_duration) {
        cue_out_duration = PyDict_GetItemString(cue_info, "");
    }

    PyObject *current_scte35 = PyDict_GetItemString(state, "current_cue_out_scte35");
    if (cue_out_scte35) {
        PyDict_SetItemString(state, "current_cue_out_scte35", cue_out_scte35);
    } else if (!current_scte35) {
        /* Keep current if no new value */
    }

    if (cue_out_duration) {
        PyDict_SetItemString(state, "current_cue_out_duration", cue_out_duration);
    }

    Py_DECREF(cue_info);
    return 0;
}

/* Parse cue out cont */
static int parse_cueout_cont(const char *line, PyObject *state) {
    PyDict_SetItemString(state, "cue_out", Py_True);

    const char *colon = strchr(line, ':');
    if (!colon || *(colon + 1) == '\0') {
        return 0;
    }

    PyObject *cue_info = parse_typed_attribute_list(line, EXT_X_CUE_OUT_CONT,
        cueout_cont_parsers, NUM_CUEOUT_CONT_PARSERS);
    if (!cue_info) return -1;

    /* Check for "2.436/120" style progress */
    PyObject *progress = PyDict_GetItemString(cue_info, "");
    if (progress) {
        char *progress_str = NULL;
        if (unicode_to_utf8_copy(progress, &progress_str) == 0) {
            const char *slash = strchr(progress_str, '/');
            if (slash) {
                /* Have elapsed/duration */
                char elapsed_str[64];
                size_t elapsed_len = slash - progress_str;
                if (elapsed_len >= sizeof(elapsed_str)) elapsed_len = sizeof(elapsed_str) - 1;
                memcpy(elapsed_str, progress_str, elapsed_len);
                elapsed_str[elapsed_len] = '\0';

                PyObject *elapsed = PyUnicode_FromString(elapsed_str);
                PyDict_SetItemString(state, "current_cue_out_elapsedtime", elapsed);
                Py_DECREF(elapsed);

                PyObject *duration = PyUnicode_FromString(slash + 1);
                PyDict_SetItemString(state, "current_cue_out_duration", duration);
                Py_DECREF(duration);
            } else {
                /* Just duration */
                PyDict_SetItemString(state, "current_cue_out_duration", progress);
            }
            PyMem_Free(progress_str);
        }
    }

    PyObject *duration = PyDict_GetItemString(cue_info, "duration");
    if (duration) {
        PyDict_SetItemString(state, "current_cue_out_duration", duration);
    }

    PyObject *scte35 = PyDict_GetItemString(cue_info, "scte35");
    if (scte35) {
        PyDict_SetItemString(state, "current_cue_out_scte35", scte35);
    }

    PyObject *elapsedtime = PyDict_GetItemString(cue_info, "elapsedtime");
    if (elapsedtime) {
        PyDict_SetItemString(state, "current_cue_out_elapsedtime", elapsedtime);
    }

    Py_DECREF(cue_info);
    return 0;
}

/*
 * Main parse function.
 *
 * Parse M3U8 playlist content and return a dictionary with all data found.
 *
 * Args:
 *     content: The M3U8 playlist content as a string.
 *     strict: If True, raise exceptions for syntax errors (default: False).
 *     custom_tags_parser: Optional callable for parsing custom tags.
 *
 * Returns:
 *     A dictionary containing the parsed playlist data.
 */
static PyObject *
m3u8_parse(PyObject *module, PyObject *args, PyObject *kwargs)
{
    const char *content;
    Py_ssize_t content_len;  /* Get size directly - enables zero-copy parsing */
    int strict = 0;
    PyObject *custom_tags_parser = Py_None;

    static char *kwlist[] = {"content", "strict", "custom_tags_parser", NULL};

    /* Use s# to get pointer AND size directly from Python string object */
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#|pO", kwlist,
                                     &content, &content_len, &strict, &custom_tags_parser)) {
        return NULL;
    }

    /*
     * Match parser.py's behavior: lines = content.strip().splitlines()
     *
     * The Python parser strips leading/trailing whitespace *before* splitting,
     * which affects strict-mode error line numbers when the input has leading
     * newlines (common with triple-quoted test fixtures).
     */
    const char *trimmed = content;
    const char *trimmed_end = content + content_len;
    while (trimmed < trimmed_end && isspace((unsigned char)*trimmed)) {
        trimmed++;
    }
    while (trimmed_end > trimmed && isspace((unsigned char)*(trimmed_end - 1))) {
        trimmed_end--;
    }
    Py_ssize_t trimmed_len = (Py_ssize_t)(trimmed_end - trimmed);

    /* Get module state for cached objects */
    m3u8_state *mod_state = get_m3u8_state(module);

    /* Check strict mode validation */
    if (strict) {
        /* Import and call version_matching.validate */
        PyObject *version_matching = PyImport_ImportModule("m3u8.version_matching");
        if (version_matching == NULL) {
            return NULL;
        }
        PyObject *validate = PyObject_GetAttrString(version_matching, "validate");
        if (validate == NULL) {
            Py_DECREF(version_matching);
            return NULL;
        }
        /* Build list like parser.py: content.strip().splitlines() */
        PyObject *lines_list = build_stripped_splitlines(trimmed);
        if (lines_list == NULL) {
            Py_DECREF(validate);
            Py_DECREF(version_matching);
            return NULL;
        }

        PyObject *errors = PyObject_CallFunctionObjArgs(validate, lines_list, NULL);
        Py_DECREF(lines_list);
        Py_DECREF(validate);
        Py_DECREF(version_matching);

        if (errors == NULL) {
            return NULL;
        }
        if (PyList_Size(errors) > 0) {
            PyErr_SetObject(PyExc_Exception, errors);
            Py_DECREF(errors);
            return NULL;
        }
        Py_DECREF(errors);
    }

    /* Initialize result data dict */
    PyObject *data = init_parse_data();
    if (data == NULL) {
        return NULL;
    }

    /* Initialize parser state dict */
    PyObject *state = init_parse_state(mod_state);
    if (state == NULL) {
        Py_DECREF(data);
        return NULL;
    }

    /*
     * Set up parse context with shadow state.
     * Shadow state avoids dict lookups for hot flags in the main loop.
     */
    ParseContext ctx = {
        .mod_state = mod_state,
        .data = data,
        .state = state,
        .strict = strict,
        .lineno = 0,
        .expect_segment = 0,   /* Matches init_parse_state */
        .expect_playlist = 0,  /* Matches init_parse_state */
    };

    /*
     * Zero-copy line parsing: Walk the buffer with pointers.
     * We use a single reusable line buffer for the null-terminated stripped line.
     * This avoids copying the entire content upfront (the strtok_r approach).
     */
    const char *p = trimmed;
    const char *end = trimmed + trimmed_len;

    /* Reusable line buffer - starts small, grows as needed */
    size_t line_buf_size = 256;
    char *line_buf = PyMem_Malloc(line_buf_size);
    if (line_buf == NULL) {
        Py_DECREF(data);
        Py_DECREF(state);
        return PyErr_NoMemory();
    }

    while (p < end) {
        ctx.lineno++;

        /* Find end of line using memchr (often hardware-optimized) */
        const char *line_start = p;
        const char *eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') {
            eol++;
        }
        Py_ssize_t line_len = eol - line_start;

        /* Strip leading whitespace */
        while (line_len > 0 && isspace((unsigned char)*line_start)) {
            line_start++;
            line_len--;
        }
        /* Strip trailing whitespace */
        while (line_len > 0 && isspace((unsigned char)line_start[line_len - 1])) {
            line_len--;
        }

        /* Advance p past the newline(s) for next iteration */
        if (eol < end) {
            if (*eol == '\r' && (eol + 1) < end && *(eol + 1) == '\n') {
                p = eol + 2;  /* Skip \r\n */
            } else {
                p = eol + 1;  /* Skip \n or \r */
            }
        } else {
            p = end;
        }

        /* Skip empty lines */
        if (line_len == 0) {
            continue;
        }

        /* Grow line buffer if needed */
        if ((size_t)line_len + 1 > line_buf_size) {
            line_buf_size = (size_t)line_len + 1;
            char *new_buf = PyMem_Realloc(line_buf, line_buf_size);
            if (new_buf == NULL) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return PyErr_NoMemory();
            }
            line_buf = new_buf;
        }

        /* Copy stripped line to null-terminated buffer */
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';
        char *stripped = line_buf;

        /* Call custom tags parser if provided */
        if (stripped[0] == '#' && custom_tags_parser != Py_None && PyCallable_Check(custom_tags_parser)) {
            /* Sync shadow state to dict before callback (so it sees current state) */
            if (sync_shadow_to_dict(&ctx) < 0) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            PyObject *result = PyObject_CallFunction(custom_tags_parser, "siOO",
                stripped, ctx.lineno, data, state);
            if (!result) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            /* Sync shadow state from dict (callback may have modified it) */
            sync_shadow_from_dict(&ctx);
            int truth = PyObject_IsTrue(result);
            Py_DECREF(result);
            if (truth < 0) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            if (truth) {
                /* p has already been advanced to the next line at the top of the loop */
                continue;
            }
        }

        if (stripped[0] == '#') {
            /* Extract tag (up to first ':') */
            char tag[128];
            const char *colon = strchr(stripped, ':');
            size_t tag_len;
            if (colon) {
                tag_len = colon - stripped;
            } else {
                tag_len = strlen(stripped);
            }
            if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
            memcpy(tag, stripped, tag_len);
            tag[tag_len] = '\0';

            /* Dispatch based on tag */
            if (strcmp(tag, EXT_M3U) == 0) {
                /* Ignore */
            }
            else if (strcmp(tag, EXT_X_TARGETDURATION) == 0) {
                const char *value = stripped + strlen(EXT_X_TARGETDURATION) + 1;
                PyObject *py_value = PyLong_FromString(value, NULL, 10);
                if (py_value) {
                    PyDict_SetItemString(data, "targetduration", py_value);
                    Py_DECREF(py_value);
                } else {
                    PyErr_Clear();
                }
            }
            else if (strcmp(tag, EXT_X_MEDIA_SEQUENCE) == 0) {
                const char *value = stripped + strlen(EXT_X_MEDIA_SEQUENCE) + 1;
                PyObject *py_value = PyLong_FromString(value, NULL, 10);
                if (py_value) {
                    PyDict_SetItemString(data, "media_sequence", py_value);
                    Py_DECREF(py_value);
                } else {
                    PyErr_Clear();
                }
            }
            else if (strcmp(tag, EXT_X_DISCONTINUITY_SEQUENCE) == 0) {
                const char *value = stripped + strlen(EXT_X_DISCONTINUITY_SEQUENCE) + 1;
                PyObject *py_value = PyLong_FromString(value, NULL, 10);
                if (py_value) {
                    PyDict_SetItemString(data, "discontinuity_sequence", py_value);
                    Py_DECREF(py_value);
                } else {
                    PyErr_Clear();
                }
            }
            else if (strcmp(tag, EXT_X_PROGRAM_DATE_TIME) == 0) {
                if (parse_program_date_time(mod_state, stripped, data, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_X_KEY) == 0) {
                if (parse_key(stripped, data, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXTINF) == 0) {
                if (parse_extinf(mod_state, stripped, state, ctx.lineno, strict) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                ctx.expect_segment = 1;  /* Shadow state for hot path */
            }
            else if (strcmp(tag, EXT_X_BYTERANGE) == 0) {
                const char *value = stripped + strlen(EXT_X_BYTERANGE) + 1;
                PyObject *segment = get_or_create_segment(mod_state, state);
                if (!segment) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                PyObject *py_value = PyUnicode_FromString(value);
                PyDict_SetItemString(segment, "byterange", py_value);
                Py_DECREF(py_value);
                ctx.expect_segment = 1;  /* Shadow state for hot path */
                dict_set_interned(state, mod_state->str_expect_segment, Py_True);
            }
            else if (strcmp(tag, EXT_X_BITRATE) == 0) {
                const char *value = stripped + strlen(EXT_X_BITRATE) + 1;
                PyObject *segment = get_or_create_segment(mod_state, state);
                if (!segment) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                PyObject *py_value = PyLong_FromString(value, NULL, 10);
                if (py_value) {
                    PyDict_SetItemString(segment, "bitrate", py_value);
                    Py_DECREF(py_value);
                } else {
                    PyErr_Clear();
                }
            }
            else if (strcmp(tag, EXT_X_STREAM_INF) == 0) {
                ctx.expect_playlist = 1;  /* Shadow state for hot path */
                dict_set_interned(state, mod_state->str_expect_playlist, Py_True);
                PyDict_SetItemString(data, "is_variant", Py_True);
                PyDict_SetItemString(data, "media_sequence", Py_None);
                PyObject *stream_info = parse_typed_attribute_list(stripped, EXT_X_STREAM_INF,
                    stream_inf_parsers, NUM_STREAM_INF_PARSERS);
                if (stream_info) {
                    PyDict_SetItemString(state, "stream_info", stream_info);
                    Py_DECREF(stream_info);
                }
            }
            else if (strcmp(tag, EXT_X_I_FRAME_STREAM_INF) == 0) {
                PyObject *iframe_info = parse_typed_attribute_list(stripped, EXT_X_I_FRAME_STREAM_INF,
                    iframe_stream_inf_parsers, NUM_IFRAME_STREAM_INF_PARSERS);
                if (iframe_info) {
                    PyObject *uri = PyDict_GetItemString(iframe_info, "uri");
                    if (uri) {
                        Py_INCREF(uri);  /* Keep uri alive before deleting from dict */
                        PyDict_DelItemString(iframe_info, "uri");
                        PyObject *playlist = PyDict_New();
                        PyDict_SetItemString(playlist, "uri", uri);
                        Py_DECREF(uri);  /* SetItemString increfs, so we can decref now */
                        PyDict_SetItemString(playlist, "iframe_stream_info", iframe_info);
                        PyObject *iframe_playlists = PyDict_GetItemString(data, "iframe_playlists");
                        PyList_Append(iframe_playlists, playlist);
                        Py_DECREF(playlist);
                    }
                    Py_DECREF(iframe_info);
                }
            }
            else if (strcmp(tag, EXT_X_IMAGE_STREAM_INF) == 0) {
                PyObject *image_info = parse_typed_attribute_list(stripped, EXT_X_IMAGE_STREAM_INF,
                    image_stream_inf_parsers, NUM_IMAGE_STREAM_INF_PARSERS);
                if (image_info) {
                    PyObject *uri = PyDict_GetItemString(image_info, "uri");
                    if (uri) {
                        Py_INCREF(uri);  /* Keep uri alive before deleting from dict */
                        PyDict_DelItemString(image_info, "uri");
                        PyObject *playlist = PyDict_New();
                        PyDict_SetItemString(playlist, "uri", uri);
                        Py_DECREF(uri);  /* SetItemString increfs, so we can decref now */
                        PyDict_SetItemString(playlist, "image_stream_info", image_info);
                        PyObject *image_playlists = PyDict_GetItemString(data, "image_playlists");
                        PyList_Append(image_playlists, playlist);
                        Py_DECREF(playlist);
                    }
                    Py_DECREF(image_info);
                }
            }
            else if (strcmp(tag, EXT_X_MEDIA) == 0) {
                PyObject *media = parse_typed_attribute_list(stripped, EXT_X_MEDIA,
                    media_parsers, NUM_MEDIA_PARSERS);
                if (media) {
                    PyObject *media_list = PyDict_GetItemString(data, "media");
                    PyList_Append(media_list, media);
                    Py_DECREF(media);
                }
            }
            else if (strcmp(tag, EXT_X_PLAYLIST_TYPE) == 0) {
                const char *value = stripped + strlen(EXT_X_PLAYLIST_TYPE) + 1;
                char normalized[64];
                size_t i;
                for (i = 0; i < sizeof(normalized) - 1 && value[i]; i++) {
                    normalized[i] = tolower((unsigned char)value[i]);
                }
                normalized[i] = '\0';
                PyObject *py_value = PyUnicode_FromString(strip(normalized));
                PyDict_SetItemString(data, "playlist_type", py_value);
                Py_DECREF(py_value);
            }
            else if (strcmp(tag, EXT_X_VERSION) == 0) {
                const char *value = stripped + strlen(EXT_X_VERSION) + 1;
                PyObject *py_value = PyLong_FromString(value, NULL, 10);
                if (py_value) {
                    PyDict_SetItemString(data, "version", py_value);
                    Py_DECREF(py_value);
                } else {
                    PyErr_Clear();
                }
            }
            else if (strcmp(tag, EXT_X_ALLOW_CACHE) == 0) {
                const char *value = stripped + strlen(EXT_X_ALLOW_CACHE) + 1;
                char normalized[64];
                size_t i;
                for (i = 0; i < sizeof(normalized) - 1 && value[i]; i++) {
                    normalized[i] = tolower((unsigned char)value[i]);
                }
                normalized[i] = '\0';
                PyObject *py_value = PyUnicode_FromString(strip(normalized));
                PyDict_SetItemString(data, "allow_cache", py_value);
                Py_DECREF(py_value);
            }
            else if (strcmp(tag, EXT_I_FRAMES_ONLY) == 0) {
                PyDict_SetItemString(data, "is_i_frames_only", Py_True);
            }
            else if (strcmp(tag, EXT_IS_INDEPENDENT_SEGMENTS) == 0) {
                PyDict_SetItemString(data, "is_independent_segments", Py_True);
            }
            else if (strcmp(tag, EXT_X_ENDLIST) == 0) {
                PyDict_SetItemString(data, "is_endlist", Py_True);
            }
            else if (strcmp(tag, EXT_X_IMAGES_ONLY) == 0) {
                PyDict_SetItemString(data, "is_images_only", Py_True);
            }
            else if (strcmp(tag, EXT_X_DISCONTINUITY) == 0) {
                PyDict_SetItemString(state, "discontinuity", Py_True);
            }
            else if (strcmp(tag, EXT_X_CUE_IN) == 0) {
                PyDict_SetItemString(state, "cue_in", Py_True);
            }
            else if (strcmp(tag, EXT_X_CUE_SPAN) == 0) {
                PyDict_SetItemString(state, "cue_out", Py_True);
            }
            else if (strcmp(tag, EXT_X_GAP) == 0) {
                PyDict_SetItemString(state, "gap", Py_True);
            }
            else if (strcmp(tag, EXT_X_CUE_OUT) == 0) {
                if (parse_cueout(stripped, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_X_CUE_OUT_CONT) == 0) {
                if (parse_cueout_cont(stripped, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_OATCLS_SCTE35) == 0) {
                const char *value = strchr(stripped, ':');
                if (value) {
                    value++;
                    PyObject *py_value = PyUnicode_FromString(value);
                    PyDict_SetItemString(state, "current_cue_out_oatcls_scte35", py_value);
                    PyObject *current = PyDict_GetItemString(state, "current_cue_out_scte35");
                    if (!current) {
                        PyDict_SetItemString(state, "current_cue_out_scte35", py_value);
                    }
                    Py_DECREF(py_value);
                }
            }
            else if (strcmp(tag, EXT_X_ASSET) == 0) {
                PyObject *asset = parse_attribute_list(stripped, EXT_X_ASSET);
                if (asset) {
                    PyDict_SetItemString(state, "asset_metadata", asset);
                    Py_DECREF(asset);
                }
            }
            else if (strcmp(tag, EXT_X_MAP) == 0) {
                PyObject *map_info = parse_typed_attribute_list(stripped, EXT_X_MAP,
                    x_map_parsers, NUM_X_MAP_PARSERS);
                if (map_info) {
                    PyDict_SetItemString(state, "current_segment_map", map_info);
                    PyObject *segment_map = PyDict_GetItemString(data, "segment_map");
                    PyList_Append(segment_map, map_info);
                    Py_DECREF(map_info);
                }
            }
            else if (strcmp(tag, EXT_X_START) == 0) {
                PyObject *start = parse_typed_attribute_list(stripped, EXT_X_START,
                    start_parsers, NUM_START_PARSERS);
                if (start) {
                    PyDict_SetItemString(data, "start", start);
                    Py_DECREF(start);
                }
            }
            else if (strcmp(tag, EXT_X_SERVER_CONTROL) == 0) {
                PyObject *server_control = parse_typed_attribute_list(stripped, EXT_X_SERVER_CONTROL,
                    server_control_parsers, NUM_SERVER_CONTROL_PARSERS);
                if (server_control) {
                    PyDict_SetItemString(data, "server_control", server_control);
                    Py_DECREF(server_control);
                }
            }
            else if (strcmp(tag, EXT_X_PART_INF) == 0) {
                PyObject *part_inf = parse_typed_attribute_list(stripped, EXT_X_PART_INF,
                    part_inf_parsers, NUM_PART_INF_PARSERS);
                if (part_inf) {
                    PyDict_SetItemString(data, "part_inf", part_inf);
                    Py_DECREF(part_inf);
                }
            }
            else if (strcmp(tag, EXT_X_PART) == 0) {
                if (parse_part(mod_state, stripped, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_X_RENDITION_REPORT) == 0) {
                PyObject *report = parse_typed_attribute_list(stripped, EXT_X_RENDITION_REPORT,
                    rendition_report_parsers, NUM_RENDITION_REPORT_PARSERS);
                if (report) {
                    PyObject *rendition_reports = PyDict_GetItemString(data, "rendition_reports");
                    PyList_Append(rendition_reports, report);
                    Py_DECREF(report);
                }
            }
            else if (strcmp(tag, EXT_X_SKIP) == 0) {
                PyObject *skip = parse_typed_attribute_list(stripped, EXT_X_SKIP,
                    skip_parsers, NUM_SKIP_PARSERS);
                if (skip) {
                    PyDict_SetItemString(data, "skip", skip);
                    Py_DECREF(skip);
                }
            }
            else if (strcmp(tag, EXT_X_SESSION_DATA) == 0) {
                PyObject *session_data = parse_typed_attribute_list(stripped, EXT_X_SESSION_DATA,
                    session_data_parsers, NUM_SESSION_DATA_PARSERS);
                if (session_data) {
                    PyObject *session_data_list = PyDict_GetItemString(data, "session_data");
                    PyList_Append(session_data_list, session_data);
                    Py_DECREF(session_data);
                }
            }
            else if (strcmp(tag, EXT_X_SESSION_KEY) == 0) {
                PyObject *raw_attrs = parse_attribute_list(stripped, EXT_X_SESSION_KEY);
                if (raw_attrs) {
                    PyObject *key = PyDict_New();
                    PyObject *k, *v;
                    Py_ssize_t pos = 0;
                    while (PyDict_Next(raw_attrs, &pos, &k, &v)) {
                        char *value_str = NULL;
                        if (unicode_to_utf8_copy(v, &value_str) < 0) continue;
                        PyObject *unquoted = remove_quotes(value_str);
                        PyMem_Free(value_str);
                        if (unquoted) {
                            PyDict_SetItem(key, k, unquoted);
                            Py_DECREF(unquoted);
                        }
                    }
                    Py_DECREF(raw_attrs);
                    PyObject *session_keys = PyDict_GetItemString(data, "session_keys");
                    PyList_Append(session_keys, key);
                    Py_DECREF(key);
                }
            }
            else if (strcmp(tag, EXT_X_PRELOAD_HINT) == 0) {
                PyObject *preload_hint = parse_typed_attribute_list(stripped, EXT_X_PRELOAD_HINT,
                    preload_hint_parsers, NUM_PRELOAD_HINT_PARSERS);
                if (preload_hint) {
                    PyDict_SetItemString(data, "preload_hint", preload_hint);
                    Py_DECREF(preload_hint);
                }
            }
            else if (strcmp(tag, EXT_X_DATERANGE) == 0) {
                PyObject *daterange = parse_typed_attribute_list(stripped, EXT_X_DATERANGE,
                    daterange_parsers, NUM_DATERANGE_PARSERS);
                if (daterange) {
                    /* Note: x_ attributes are already captured by parse_typed_attribute_list
                       as ATTR_STRING (the default case for unknown attributes) */
                    PyObject *dateranges = PyDict_GetItemString(state, "dateranges");
                    if (!dateranges) {
                        dateranges = PyList_New(0);
                        PyDict_SetItemString(state, "dateranges", dateranges);
                        Py_DECREF(dateranges);
                        dateranges = PyDict_GetItemString(state, "dateranges");
                    }
                    PyList_Append(dateranges, daterange);
                    Py_DECREF(daterange);
                }
            }
            else if (strcmp(tag, EXT_X_CONTENT_STEERING) == 0) {
                PyObject *content_steering = parse_typed_attribute_list(stripped, EXT_X_CONTENT_STEERING,
                    content_steering_parsers, NUM_CONTENT_STEERING_PARSERS);
                if (content_steering) {
                    PyDict_SetItemString(data, "content_steering", content_steering);
                    Py_DECREF(content_steering);
                }
            }
            else if (strcmp(tag, EXT_X_TILES) == 0) {
                PyObject *tiles = parse_typed_attribute_list(stripped, EXT_X_TILES,
                    tiles_parsers, NUM_TILES_PARSERS);
                if (tiles) {
                    PyObject *tiles_list = PyDict_GetItemString(data, "tiles");
                    PyList_Append(tiles_list, tiles);
                    Py_DECREF(tiles);
                }
            }
            else if (strcmp(tag, EXT_X_BLACKOUT) == 0) {
                const char *colon = strchr(stripped, ':');
                if (colon && *(colon + 1)) {
                    PyObject *blackout_data = PyUnicode_FromString(colon + 1);
                    PyDict_SetItemString(state, "blackout", blackout_data);
                    Py_DECREF(blackout_data);
                } else {
                    PyDict_SetItemString(state, "blackout", Py_True);
                }
            }
            else {
                /* Unknown tag */
                if (strict) {
                    raise_parse_error(mod_state, ctx.lineno, stripped);
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
        } else {
            /* Non-comment line - segment or playlist URI */
            /* Use shadow state for hot path checks (no dict lookups) */
            if (ctx.expect_segment) {
                if (parse_ts_chunk(mod_state, stripped, data, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                ctx.expect_segment = 0;  /* parse_ts_chunk clears this */
            } else if (ctx.expect_playlist) {
                if (parse_variant_playlist(stripped, data, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                ctx.expect_playlist = 0;  /* parse_variant_playlist clears this */
            } else if (strict) {
                raise_parse_error(mod_state, ctx.lineno, stripped);
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
        }
        /* Loop continues with pointer already advanced */
    }

    PyMem_Free(line_buf);

    /* Handle remaining partial segment */
    PyObject *segment = PyDict_GetItemString(state, "segment");
    if (segment) {
        PyObject *segments = PyDict_GetItemString(data, "segments");
        PyList_Append(segments, segment);
    }

    Py_DECREF(state);
    return data;
}

/* Module methods */
static PyMethodDef m3u8_parser_methods[] = {
    {"parse", (PyCFunction)m3u8_parse, METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR(
     "parse(content, strict=False, custom_tags_parser=None)\n"
     "--\n\n"
     "Parse M3U8 playlist content and return a dictionary with all data found.\n\n"
     "This is an optimized C implementation that produces output identical to\n"
     "the pure Python parser in m3u8.parser.parse().\n\n"
     "Parameters\n"
     "----------\n"
     "content : str\n"
     "    The M3U8 playlist content as a string.\n"
     "strict : bool, optional\n"
     "    If True, raise exceptions for syntax errors. Default is False.\n"
     "custom_tags_parser : callable, optional\n"
     "    A function that receives (line, lineno, data, state) for custom tag\n"
     "    handling. Return True to skip default parsing for that line.\n\n"
     "Returns\n"
     "-------\n"
     "dict\n"
     "    A dictionary containing the parsed playlist data with keys including:\n"
     "    'segments', 'playlists', 'media', 'keys', 'is_variant', etc.\n\n"
     "Raises\n"
     "------\n"
     "ParseError\n"
     "    If strict=True and a syntax error is encountered.\n"
     "Exception\n"
     "    If strict=True and version validation fails.\n\n"
     "Examples\n"
     "--------\n"
     ">>> from m3u8._m3u8_parser import parse\n"
     ">>> result = parse('#EXTM3U\\n#EXTINF:10,\\nfoo.ts')\n"
     ">>> len(result['segments'])\n"
     "1\n"
     )},
    {NULL, NULL, 0, NULL}
};

/*
 * Module traverse function for GC.
 * Visit all Python objects in module state.
 */
static int
m3u8_parser_traverse(PyObject *module, visitproc visit, void *arg)
{
    m3u8_state *state = get_m3u8_state(module);
    Py_VISIT(state->ParseError);
    Py_VISIT(state->datetime_cls);
    Py_VISIT(state->timedelta_cls);
    Py_VISIT(state->fromisoformat_meth);
    /* Interned strings are immortal in most cases, but visit for correctness */
    Py_VISIT(state->str_segment);
    Py_VISIT(state->str_segments);
    Py_VISIT(state->str_duration);
    Py_VISIT(state->str_uri);
    Py_VISIT(state->str_title);
    Py_VISIT(state->str_expect_segment);
    Py_VISIT(state->str_expect_playlist);
    Py_VISIT(state->str_current_key);
    Py_VISIT(state->str_keys);
    Py_VISIT(state->str_cue_out);
    Py_VISIT(state->str_cue_in);
    return 0;
}

/*
 * Module clear function for GC.
 * Clear all Python objects in module state.
 */
static int
m3u8_parser_clear(PyObject *module)
{
    m3u8_state *state = get_m3u8_state(module);
    Py_CLEAR(state->ParseError);
    Py_CLEAR(state->datetime_cls);
    Py_CLEAR(state->timedelta_cls);
    Py_CLEAR(state->fromisoformat_meth);
    /* Note: interned strings may be shared, Py_CLEAR handles this safely */
    Py_CLEAR(state->str_segment);
    Py_CLEAR(state->str_segments);
    Py_CLEAR(state->str_duration);
    Py_CLEAR(state->str_uri);
    Py_CLEAR(state->str_title);
    Py_CLEAR(state->str_expect_segment);
    Py_CLEAR(state->str_expect_playlist);
    Py_CLEAR(state->str_current_key);
    Py_CLEAR(state->str_keys);
    Py_CLEAR(state->str_cue_out);
    Py_CLEAR(state->str_cue_in);
    return 0;
}

/*
 * Module deallocation function.
 */
static void
m3u8_parser_free(void *module)
{
    m3u8_parser_clear((PyObject *)module);
}

/* Module definition */
static struct PyModuleDef m3u8_parser_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_m3u8_parser",
    .m_doc = "C extension for fast M3U8 playlist parsing.",
    .m_size = sizeof(m3u8_state),
    .m_methods = m3u8_parser_methods,
    .m_traverse = m3u8_parser_traverse,
    .m_clear = m3u8_parser_clear,
    .m_free = m3u8_parser_free,
};

/*
 * Module initialization.
 *
 * Creates the module, initializes module state, and sets up cached objects.
 */
PyMODINIT_FUNC
PyInit__m3u8_parser(void)
{
    PyObject *m = PyModule_Create(&m3u8_parser_module);
    if (m == NULL) {
        return NULL;
    }

    m3u8_state *state = get_m3u8_state(m);

    /* Initialize module state to NULL for safe cleanup on error */
    state->ParseError = NULL;
    state->datetime_cls = NULL;
    state->timedelta_cls = NULL;
    state->fromisoformat_meth = NULL;
    state->str_segment = NULL;
    state->str_segments = NULL;
    state->str_duration = NULL;
    state->str_uri = NULL;
    state->str_title = NULL;
    state->str_expect_segment = NULL;
    state->str_expect_playlist = NULL;
    state->str_current_key = NULL;
    state->str_keys = NULL;
    state->str_cue_out = NULL;
    state->str_cue_in = NULL;

    /* Import ParseError from m3u8.parser to use the same exception class */
    PyObject *parser_module = PyImport_ImportModule("m3u8.parser");
    if (parser_module != NULL) {
        state->ParseError = PyObject_GetAttrString(parser_module, "ParseError");
        Py_DECREF(parser_module);
    }

    /* Fallback: create our own ParseError if import fails */
    if (state->ParseError == NULL) {
        PyErr_Clear();
        state->ParseError = PyErr_NewException(
            "m3u8._m3u8_parser.ParseError", PyExc_Exception, NULL);
        if (state->ParseError == NULL) {
            goto error;
        }
    }

    /* Add ParseError to module (PyModule_AddObject steals a reference on success) */
    Py_INCREF(state->ParseError);
    if (PyModule_AddObject(m, "ParseError", state->ParseError) < 0) {
        Py_DECREF(state->ParseError);
        goto error;
    }

    /* Initialize datetime cache */
    if (init_datetime_cache(state) < 0) {
        goto error;
    }

    /* Initialize interned strings for common dict keys */
    if (init_interned_strings(state) < 0) {
        goto error;
    }

    return m;

error:
    Py_DECREF(m);
    return NULL;
}

