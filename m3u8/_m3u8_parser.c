/*
 * Copyright 2014 Globo.com Player authors. All rights reserved.
 * Use of this source code is governed by a MIT License
 * license that can be found in the LICENSE file.
 *
 * C extension for m3u8 parser - provides optimized parsing of M3U8 playlists.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* Cross-platform compatibility for Windows/MSVC */
#ifdef _WIN32
#define strdup _strdup
#define strtok_r strtok_s
#endif

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

/* Forward declarations */
static PyObject *ParseError = NULL;

/* Cached datetime objects for performance */
static PyObject *datetime_cls = NULL;
static PyObject *timedelta_cls = NULL;
static PyObject *fromisoformat_meth = NULL;

/* Helper to ensure datetime module is loaded (lazy initialization) */
static int load_datetime_module(void) {
    if (datetime_cls && timedelta_cls && fromisoformat_meth) return 0;

    PyObject *datetime_mod = PyImport_ImportModule("datetime");
    if (!datetime_mod) return -1;

    datetime_cls = PyObject_GetAttrString(datetime_mod, "datetime");
    timedelta_cls = PyObject_GetAttrString(datetime_mod, "timedelta");

    if (datetime_cls) {
        fromisoformat_meth = PyObject_GetAttrString(datetime_cls, "fromisoformat");
    }

    Py_DECREF(datetime_mod);

    if (!datetime_cls || !timedelta_cls || !fromisoformat_meth) {
        Py_XDECREF(datetime_cls);
        Py_XDECREF(timedelta_cls);
        Py_XDECREF(fromisoformat_meth);
        datetime_cls = NULL;
        timedelta_cls = NULL;
        fromisoformat_meth = NULL;
        return -1;
    }
    return 0;
}

/* Utility: raise ParseError with lineno and line arguments */
static void raise_parse_error(int lineno, const char *line) {
    PyObject *args = Py_BuildValue("(is)", lineno, line);
    if (args) {
        PyObject *exc = PyObject_Call(ParseError, args, NULL);
        Py_DECREF(args);
        if (exc) {
            PyErr_SetObject(ParseError, exc);
            Py_DECREF(exc);
        }
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

/* Utility: strip whitespace from string */
static char *strip(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
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

/* Utility: dt + timedelta(seconds=secs) */
static PyObject *datetime_add_seconds(PyObject *dt, double secs) {
    if (load_datetime_module() < 0) return NULL;

    PyObject *args = PyTuple_New(0);
    if (!args) return NULL;

    PyObject *kwargs = Py_BuildValue("{s:d}", "seconds", secs);
    if (!kwargs) {
        Py_DECREF(args);
        return NULL;
    }

    PyObject *delta = PyObject_Call(timedelta_cls, args, kwargs);
    Py_DECREF(kwargs);
    Py_DECREF(args);
    if (!delta) return NULL;

    PyObject *new_dt = PyNumber_Add(dt, delta);
    Py_DECREF(delta);
    return new_dt;
}

/* Utility: normalize attribute name (replace - with _, lowercase) */
static void normalize_attribute(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '-') {
            dst[i] = '_';
        } else {
            dst[i] = tolower((unsigned char)*src);
        }
        src++;
        i++;
    }
    /* Strip trailing whitespace */
    while (i > 0 && isspace((unsigned char)dst[i-1])) i--;
    dst[i] = '\0';
    /* Strip leading whitespace */
    char *start = dst;
    while (isspace((unsigned char)*start)) start++;
    if (start != dst) {
        memmove(dst, start, strlen(start) + 1);
    }
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

/* Parse attribute list from a line like "KEY=value,KEY2=value2" */
static PyObject *parse_attribute_list(const char *line, const char *prefix) {
    PyObject *attrs = PyDict_New();
    if (!attrs) return NULL;

    /* Skip prefix if present */
    const char *content = line;
    if (prefix) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) == 0) {
            content = line + prefix_len;
            if (*content == ':') content++;
        } else {
            return attrs;  /* Return empty dict if prefix not found */
        }
    }

    /* Parse attributes */
    const char *p = content;
    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* Find attribute name */
        const char *name_start = p;
        while (*p && *p != '=' && *p != ',') p++;

        if (*p == '=') {
            /* Have name=value pair */
            size_t name_len = p - name_start;
            char *name = malloc(name_len + 1);
            if (!name) {
                Py_DECREF(attrs);
                return PyErr_NoMemory();
            }
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';

            /* Use dynamic allocation for long attribute names to avoid truncation */
            char stack_buf[256];
            char *normalized_name;
            char *heap_buf = NULL;
            if (name_len < sizeof(stack_buf)) {
                normalized_name = stack_buf;
                normalize_attribute(name, normalized_name, sizeof(stack_buf));
            } else {
                heap_buf = malloc(name_len + 1);
                if (!heap_buf) {
                    free(name);
                    Py_DECREF(attrs);
                    return PyErr_NoMemory();
                }
                normalized_name = heap_buf;
                normalize_attribute(name, normalized_name, name_len + 1);
            }
            free(name);

            p++;  /* Skip '=' */

            /* Parse value - keep quotes for quoted values */
            const char *value_start = p;
            if (*p == '"' || *p == '\'') {
                /* Quoted value - include the quotes in the value */
                char quote = *p;
                const char *quote_start = p;
                p++;  /* Skip opening quote */
                while (*p && *p != quote) p++;
                if (*p == quote) p++;  /* Skip closing quote */
                size_t value_len = p - quote_start;

                char *value = malloc(value_len + 1);
                if (!value) {
                    free(heap_buf);
                    Py_DECREF(attrs);
                    return PyErr_NoMemory();
                }
                memcpy(value, quote_start, value_len);
                value[value_len] = '\0';

                PyObject *py_value = PyUnicode_FromString(value);
                free(value);
                if (!py_value) {
                    free(heap_buf);
                    Py_DECREF(attrs);
                    return NULL;
                }
                PyDict_SetItemString(attrs, normalized_name, py_value);
                Py_DECREF(py_value);
            } else {
                /* Unquoted value - find end at comma or end of string */
                while (*p && *p != ',') p++;
                size_t value_len = p - value_start;
                char *value = malloc(value_len + 1);
                if (!value) {
                    free(heap_buf);
                    Py_DECREF(attrs);
                    return PyErr_NoMemory();
                }
                memcpy(value, value_start, value_len);
                value[value_len] = '\0';

                PyObject *py_value = PyUnicode_FromString(value);
                free(value);
                if (!py_value) {
                    free(heap_buf);
                    Py_DECREF(attrs);
                    return NULL;
                }
                PyDict_SetItemString(attrs, normalized_name, py_value);
                Py_DECREF(py_value);
            }
            free(heap_buf);  /* Safe to call on NULL */
        } else {
            /* Just a value without name */
            size_t value_len = p - name_start;
            if (value_len > 0) {
                char *value = malloc(value_len + 1);
                if (!value) {
                    Py_DECREF(attrs);
                    return PyErr_NoMemory();
                }
                memcpy(value, name_start, value_len);
                value[value_len] = '\0';

                /* Trim whitespace */
                char *trimmed = strip(value);
                if (*trimmed) {
                    PyObject *py_value = PyUnicode_FromString(trimmed);
                    if (!py_value) {
                        free(value);
                        Py_DECREF(attrs);
                        return NULL;
                    }
                    PyDict_SetItemString(attrs, "", py_value);
                    Py_DECREF(py_value);
                }
                free(value);
            }
        }

        /* Skip comma */
        if (*p == ',') p++;
    }

    return attrs;
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

static PyObject *parse_typed_attribute_list(const char *line, const char *prefix,
                                            const AttrParser *parsers, size_t num_parsers) {
    PyObject *raw_attrs = parse_attribute_list(line, prefix);
    if (!raw_attrs) return NULL;

    PyObject *attrs = PyDict_New();
    if (!attrs) {
        Py_DECREF(raw_attrs);
        return NULL;
    }

    /* Copy and convert attributes */
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(raw_attrs, &pos, &key, &value)) {
        AttrType type = ATTR_STRING;

        /* Find parser for this attribute */
        for (size_t i = 0; i < num_parsers; i++) {
            if (PyUnicode_Check(key) && PyUnicode_CompareWithASCIIString(key, parsers[i].name) == 0) {
                type = parsers[i].type;
                break;
            }
        }

        PyObject *converted = NULL;
        char *value_str = NULL;
        if (unicode_to_utf8_copy(value, &value_str) < 0) {
            /* Abort on conversion failure to propagate the exception */
            Py_DECREF(attrs);
            Py_DECREF(raw_attrs);
            return NULL;
        }

        switch (type) {
            case ATTR_INT: {
                converted = PyLong_FromString(value_str, NULL, 10);
                if (!converted) PyErr_Clear();  /* Fall back to keeping as string */
                break;
            }
            case ATTR_FLOAT: {
                double v = PyOS_string_to_double(value_str, NULL, NULL);
                if (v == -1.0 && PyErr_Occurred()) {
                    PyErr_Clear();
                    converted = NULL;
                } else {
                    converted = PyFloat_FromDouble(v);
                }
                break;
            }
            case ATTR_BANDWIDTH: {
                /* bandwidth can be float but should be truncated to int */
                double v = PyOS_string_to_double(value_str, NULL, NULL);
                if (v == -1.0 && PyErr_Occurred()) {
                    PyErr_Clear();
                    converted = NULL;
                } else {
                    converted = PyLong_FromDouble(v);
                }
                break;
            }
            case ATTR_QUOTED_STRING:
                /* Remove quotes from quoted values */
                converted = remove_quotes(value_str);
                break;
            case ATTR_STRING:
            default:
                Py_INCREF(value);
                converted = value;
                break;
        }
        PyMem_Free(value_str);

        if (converted) {
            PyDict_SetItem(attrs, key, converted);
            Py_DECREF(converted);
        }
    }

    Py_DECREF(raw_attrs);
    return attrs;
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

/* Parse EXTINF */
static int parse_extinf(const char *line, PyObject *state, int lineno, int strict) {
    const char *content = line + strlen(EXTINF) + 1;  /* Skip "#EXTINF:" */

    /* Find comma separator */
    const char *comma = strchr(content, ',');
    double duration;
    const char *title = "";

    if (comma) {
        char duration_str[64];
        size_t dur_len = comma - content;
        if (dur_len >= sizeof(duration_str)) dur_len = sizeof(duration_str) - 1;
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
            raise_parse_error(lineno, line);
            return -1;
        }
        duration = PyOS_string_to_double(content, NULL, NULL);
        if (duration == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            duration = 0.0;
        }
    }

    /* Get or create segment dict in state */
    PyObject *segment = PyDict_GetItemString(state, "segment");
    if (!segment) {
        segment = PyDict_New();
        if (!segment) return -1;
        PyDict_SetItemString(state, "segment", segment);
        Py_DECREF(segment);
        segment = PyDict_GetItemString(state, "segment");
    }

    PyObject *py_duration = PyFloat_FromDouble(duration);
    PyDict_SetItemString(segment, "duration", py_duration);
    Py_DECREF(py_duration);

    PyObject *py_title = PyUnicode_FromString(title);
    PyDict_SetItemString(segment, "title", py_title);
    Py_DECREF(py_title);

    PyDict_SetItemString(state, "expect_segment", Py_True);
    return 0;
}

/* Parse a segment (ts chunk) */
static int parse_ts_chunk(const char *line, PyObject *data, PyObject *state) {
    PyObject *segment = PyDict_GetItemString(state, "segment");
    if (!segment) {
        segment = PyDict_New();
        if (!segment) return -1;
    } else {
        Py_INCREF(segment);
    }
    if (del_item_string_ignore_keyerror(state, "segment") < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* Add URI */
    PyObject *uri = PyUnicode_FromString(line);
    PyDict_SetItemString(segment, "uri", uri);
    Py_DECREF(uri);

    /* Transfer state values to segment */
    PyObject *pdt = PyDict_GetItemString(state, "program_date_time");
    if (pdt) {
        PyDict_SetItemString(segment, "program_date_time", pdt);
        PyDict_DelItemString(state, "program_date_time");
    }

    PyObject *current_pdt = PyDict_GetItemString(state, "current_program_date_time");
    if (current_pdt) {
        PyDict_SetItemString(segment, "current_program_date_time", current_pdt);
        /* Update current_program_date_time by adding duration */
        PyObject *duration = PyDict_GetItemString(segment, "duration");
        if (duration && current_pdt) {
            double secs = PyFloat_AsDouble(duration);
            if (PyErr_Occurred()) return -1;
            PyObject *new_pdt = datetime_add_seconds(current_pdt, secs);
            if (!new_pdt) return -1;
            PyDict_SetItemString(state, "current_program_date_time", new_pdt);
            Py_DECREF(new_pdt);
        }
    }

    /* Boolean flags from state */
    PyObject *cue_in = PyDict_GetItemString(state, "cue_in");
    PyDict_SetItemString(segment, "cue_in", cue_in ? Py_True : Py_False);
    if (cue_in) PyDict_DelItemString(state, "cue_in");

    PyObject *cue_out = PyDict_GetItemString(state, "cue_out");
    int cue_out_truth = cue_out ? PyObject_IsTrue(cue_out) : 0;
    if (cue_out_truth < 0) return -1;
    PyDict_SetItemString(segment, "cue_out", cue_out_truth ? Py_True : Py_False);

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

    /* Key */
    PyObject *current_key = PyDict_GetItemString(state, "current_key");
    if (current_key) {
        PyDict_SetItemString(segment, "key", current_key);
    } else {
        /* For unencrypted segments, ensure None is in keys list */
        PyObject *keys = PyDict_GetItemString(data, "keys");
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

    /* Add to segments */
    PyObject *segments = PyDict_GetItemString(data, "segments");
    if (segments) {
        PyList_Append(segments, segment);
    }

    PyDict_SetItemString(state, "expect_segment", Py_False);
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

/* Parse program date time */
static int parse_program_date_time(const char *line, PyObject *data, PyObject *state) {
    const char *value = strchr(line, ':');
    if (!value) return 0;
    value++;

    if (load_datetime_module() < 0) return -1;

    PyObject *dt = PyObject_CallFunction(fromisoformat_meth, "s", value);
    if (!dt) return -1;

    /* Set in data if not already set */
    PyObject *existing = PyDict_GetItemString(data, "program_date_time");
    if (!existing || existing == Py_None) {
        PyDict_SetItemString(data, "program_date_time", dt);
    }

    PyDict_SetItemString(state, "current_program_date_time", dt);
    PyDict_SetItemString(state, "program_date_time", dt);
    Py_DECREF(dt);
    return 0;
}

/* Parse part */
static int parse_part(const char *line, PyObject *state) {
    PyObject *part = parse_typed_attribute_list(line, EXT_X_PART, part_parsers, NUM_PART_PARSERS);
    if (!part) return -1;

    /* Add program_date_time if available */
    PyObject *current_pdt = PyDict_GetItemString(state, "current_program_date_time");
    if (current_pdt) {
        PyDict_SetItemString(part, "program_date_time", current_pdt);
        /* Update current_program_date_time */
        PyObject *duration = PyDict_GetItemString(part, "duration");
        if (duration) {
            double secs = PyFloat_AsDouble(duration);
            if (PyErr_Occurred()) {
                Py_DECREF(part);
                return -1;
            }
            PyObject *new_pdt = datetime_add_seconds(current_pdt, secs);
            if (!new_pdt) {
                Py_DECREF(part);
                return -1;
            }
            PyDict_SetItemString(state, "current_program_date_time", new_pdt);
            Py_DECREF(new_pdt);
        }
    }

    /* Add dateranges */
    PyObject *dateranges = PyDict_GetItemString(state, "dateranges");
    if (dateranges) {
        PyDict_SetItemString(part, "dateranges", dateranges);
        PyDict_DelItemString(state, "dateranges");
    } else {
        PyDict_SetItemString(part, "dateranges", Py_None);
    }

    /* Add gap_tag */
    PyObject *gap = PyDict_GetItemString(state, "gap");
    if (gap) {
        PyDict_SetItemString(part, "gap_tag", Py_True);
        PyDict_DelItemString(state, "gap");
    } else {
        PyDict_SetItemString(part, "gap_tag", Py_None);
    }

    /* Get or create segment */
    PyObject *segment = PyDict_GetItemString(state, "segment");
    if (!segment) {
        segment = PyDict_New();
        if (!segment) {
            Py_DECREF(part);
            return -1;
        }
        PyDict_SetItemString(state, "segment", segment);
        Py_DECREF(segment);
        segment = PyDict_GetItemString(state, "segment");
    }

    /* Get or create parts list in segment */
    PyObject *parts = PyDict_GetItemString(segment, "parts");
    if (!parts) {
        parts = PyList_New(0);
        if (!parts) {
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

/* Main parse function */
static PyObject *m3u8_parse(PyObject *self, PyObject *args, PyObject *kwargs) {
    const char *content;
    int strict = 0;
    PyObject *custom_tags_parser = Py_None;

    static char *kwlist[] = {"content", "strict", "custom_tags_parser", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|pO", kwlist,
                                     &content, &strict, &custom_tags_parser)) {
        return NULL;
    }

    /* Check strict mode validation */
    if (strict) {
        /* Import and call version_matching.validate */
        PyObject *version_matching = PyImport_ImportModule("m3u8.version_matching");
        if (!version_matching) return NULL;
        PyObject *validate = PyObject_GetAttrString(version_matching, "validate");
        if (!validate) {
            Py_DECREF(version_matching);
            return NULL;
        }
        /* Build list like parser.py: content.strip().splitlines() */
        PyObject *lines_list = build_stripped_splitlines(content);
        if (!lines_list) {
            Py_DECREF(validate);
            Py_DECREF(version_matching);
            return NULL;
        }

        PyObject *errors = PyObject_CallFunctionObjArgs(validate, lines_list, NULL);
        Py_DECREF(lines_list);
        Py_DECREF(validate);
        Py_DECREF(version_matching);

        if (!errors) return NULL;
        if (PyList_Size(errors) > 0) {
            PyErr_SetObject(PyExc_Exception, errors);
            Py_DECREF(errors);
            return NULL;
        }
        Py_DECREF(errors);
    }

    /* Initialize result data */
    PyObject *data = PyDict_New();
    if (!data) return NULL;

    if (set_item_string_stealref(data, "media_sequence", PyLong_FromLong(0)) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_variant", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_endlist", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_i_frames_only", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_independent_segments", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "is_images_only", Py_False) < 0) goto fail;
    if (PyDict_SetItemString(data, "playlist_type", Py_None) < 0) goto fail;

    PyObject *playlists_list = PyList_New(0);
    PyDict_SetItemString(data, "playlists", playlists_list);
    Py_DECREF(playlists_list);

    PyObject *segments_list = PyList_New(0);
    PyDict_SetItemString(data, "segments", segments_list);
    Py_DECREF(segments_list);

    PyObject *iframe_playlists_list = PyList_New(0);
    PyDict_SetItemString(data, "iframe_playlists", iframe_playlists_list);
    Py_DECREF(iframe_playlists_list);

    PyObject *image_playlists_list = PyList_New(0);
    PyDict_SetItemString(data, "image_playlists", image_playlists_list);
    Py_DECREF(image_playlists_list);

    PyObject *tiles_list = PyList_New(0);
    PyDict_SetItemString(data, "tiles", tiles_list);
    Py_DECREF(tiles_list);

    PyObject *media_list = PyList_New(0);
    PyDict_SetItemString(data, "media", media_list);
    Py_DECREF(media_list);

    PyObject *keys_list = PyList_New(0);
    PyDict_SetItemString(data, "keys", keys_list);
    Py_DECREF(keys_list);

    PyObject *rendition_reports_list = PyList_New(0);
    PyDict_SetItemString(data, "rendition_reports", rendition_reports_list);
    Py_DECREF(rendition_reports_list);

    PyObject *skip_dict = PyDict_New();
    PyDict_SetItemString(data, "skip", skip_dict);
    Py_DECREF(skip_dict);

    PyObject *part_inf_dict = PyDict_New();
    PyDict_SetItemString(data, "part_inf", part_inf_dict);
    Py_DECREF(part_inf_dict);

    PyObject *session_data_list = PyList_New(0);
    PyDict_SetItemString(data, "session_data", session_data_list);
    Py_DECREF(session_data_list);

    PyObject *session_keys_list = PyList_New(0);
    PyDict_SetItemString(data, "session_keys", session_keys_list);
    Py_DECREF(session_keys_list);

    PyObject *segment_map_list = PyList_New(0);
    PyDict_SetItemString(data, "segment_map", segment_map_list);
    Py_DECREF(segment_map_list);

    /* Initialize state */
    PyObject *state = PyDict_New();
    if (!state) {
        Py_DECREF(data);
        return NULL;
    }

    PyDict_SetItemString(state, "expect_segment", Py_False);
    PyDict_SetItemString(state, "expect_playlist", Py_False);

    /* Split content into lines and parse */
    char *content_copy = strdup(content);
    if (!content_copy) {
        Py_DECREF(data);
        Py_DECREF(state);
        return PyErr_NoMemory();
    }

    int lineno = 0;
    char *saveptr;
    char *line = strtok_r(content_copy, "\n\r", &saveptr);

    while (line) {
        lineno++;

        /* Strip whitespace */
        char *stripped = strip(line);

        /* Skip empty lines */
        if (*stripped == '\0') {
            line = strtok_r(NULL, "\n\r", &saveptr);
            continue;
        }

        /* Call custom tags parser if provided */
        if (stripped[0] == '#' && custom_tags_parser != Py_None && PyCallable_Check(custom_tags_parser)) {
            PyObject *result = PyObject_CallFunction(custom_tags_parser, "siOO",
                stripped, lineno, data, state);
            if (!result) {
                free(content_copy);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            int truth = PyObject_IsTrue(result);
            Py_DECREF(result);
            if (truth < 0) {
                free(content_copy);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            if (truth) {
                line = strtok_r(NULL, "\n\r", &saveptr);
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
                if (parse_program_date_time(stripped, data, state) < 0) {
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_X_KEY) == 0) {
                if (parse_key(stripped, data, state) < 0) {
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXTINF) == 0) {
                if (parse_extinf(stripped, state, lineno, strict) < 0) {
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_X_BYTERANGE) == 0) {
                const char *value = stripped + strlen(EXT_X_BYTERANGE) + 1;
                PyObject *segment = PyDict_GetItemString(state, "segment");
                if (!segment) {
                    segment = PyDict_New();
                    PyDict_SetItemString(state, "segment", segment);
                    Py_DECREF(segment);
                    segment = PyDict_GetItemString(state, "segment");
                }
                PyObject *py_value = PyUnicode_FromString(value);
                PyDict_SetItemString(segment, "byterange", py_value);
                Py_DECREF(py_value);
                PyDict_SetItemString(state, "expect_segment", Py_True);
            }
            else if (strcmp(tag, EXT_X_BITRATE) == 0) {
                const char *value = stripped + strlen(EXT_X_BITRATE) + 1;
                PyObject *segment = PyDict_GetItemString(state, "segment");
                if (!segment) {
                    segment = PyDict_New();
                    PyDict_SetItemString(state, "segment", segment);
                    Py_DECREF(segment);
                    segment = PyDict_GetItemString(state, "segment");
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
                PyDict_SetItemString(state, "expect_playlist", Py_True);
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
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
            else if (strcmp(tag, EXT_X_CUE_OUT_CONT) == 0) {
                if (parse_cueout_cont(stripped, state) < 0) {
                    free(content_copy);
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
                if (parse_part(stripped, state) < 0) {
                    free(content_copy);
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
                    raise_parse_error(lineno, stripped);
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
        } else {
            /* Non-comment line - segment or playlist URI */
            PyObject *expect_segment = PyDict_GetItemString(state, "expect_segment");
            PyObject *expect_playlist = PyDict_GetItemString(state, "expect_playlist");

            if (expect_segment == Py_True) {
                if (parse_ts_chunk(stripped, data, state) < 0) {
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            } else if (expect_playlist == Py_True) {
                if (parse_variant_playlist(stripped, data, state) < 0) {
                    free(content_copy);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            } else if (strict) {
                raise_parse_error(lineno, stripped);
                free(content_copy);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
        }

        line = strtok_r(NULL, "\n\r", &saveptr);
    }

    free(content_copy);

    /* Handle remaining partial segment */
    PyObject *segment = PyDict_GetItemString(state, "segment");
    if (segment) {
        PyObject *segments = PyDict_GetItemString(data, "segments");
        PyList_Append(segments, segment);
    }

    Py_DECREF(state);
    return data;

fail:
    Py_DECREF(data);
    return NULL;
}

/* Module methods */
static PyMethodDef m3u8_parser_methods[] = {
    {"parse", (PyCFunction)m3u8_parse, METH_VARARGS | METH_KEYWORDS,
     "Parse M3U8 playlist content and return a dictionary with all data found.\n\n"
     "Args:\n"
     "    content: The M3U8 playlist content as a string.\n"
     "    strict: If True, raise exceptions for syntax errors (default: False).\n"
     "    custom_tags_parser: Optional callable for parsing custom tags.\n\n"
     "Returns:\n"
     "    A dictionary containing the parsed playlist data."},
    {NULL, NULL, 0, NULL}
};

/* Module definition */
static struct PyModuleDef m3u8_parser_module = {
    PyModuleDef_HEAD_INIT,
    "_m3u8_parser",
    "C extension for fast M3U8 playlist parsing.",
    -1,
    m3u8_parser_methods
};

/* Module initialization */
PyMODINIT_FUNC PyInit__m3u8_parser(void) {
    PyObject *m = PyModule_Create(&m3u8_parser_module);
    if (m == NULL) return NULL;

    /* Import ParseError from m3u8.parser to use the same exception class */
    PyObject *parser_module = PyImport_ImportModule("m3u8.parser");
    if (parser_module) {
        ParseError = PyObject_GetAttrString(parser_module, "ParseError");
        Py_DECREF(parser_module);
        if (ParseError) {
            Py_INCREF(ParseError);
            PyModule_AddObject(m, "ParseError", ParseError);
        }
    }

    /* Fallback: create our own ParseError if import fails */
    if (ParseError == NULL) {
        PyErr_Clear();
        ParseError = PyErr_NewException("m3u8._m3u8_parser.ParseError", PyExc_Exception, NULL);
        if (ParseError == NULL) {
            Py_DECREF(m);
            return NULL;
        }
        Py_INCREF(ParseError);
        PyModule_AddObject(m, "ParseError", ParseError);
    }

    return m;
}

