#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace asura {

constexpr uint64_t KiB = 1024ull;
constexpr uint64_t MiB = 1024ull * KiB;
constexpr uint64_t GiB = 1024ull * MiB;

struct Error {
    bool set;
    char message[2048];
};

inline bool fail(Error *err, const char *fmt, ...) {
    if (err && !err->set) {
        err->set = true;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->message, sizeof(err->message), fmt, ap);
        va_end(ap);
        err->message[sizeof(err->message) - 1] = 0;
    }
    return false;
}

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

struct Arena {
    uint8_t *base;
    uint64_t reserved;
    uint64_t committed;
    uint64_t used;
};

inline bool arena_init(Arena *arena, uint64_t reserve_size, Error *err) {
    memset(arena, 0, sizeof(*arena));
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    reserve_size = align_up(reserve_size, si.dwAllocationGranularity);
    arena->base = static_cast<uint8_t *>(VirtualAlloc(nullptr, static_cast<SIZE_T>(reserve_size), MEM_RESERVE, PAGE_NOACCESS));
    if (!arena->base)
        return fail(err, "VirtualAlloc reserve failed for %llu bytes (win32=%lu)",
                    static_cast<unsigned long long>(reserve_size), GetLastError());
    arena->reserved = reserve_size;
    return true;
}

inline void arena_release(Arena *arena) {
    if (arena->base)
        VirtualFree(arena->base, 0, MEM_RELEASE);
    memset(arena, 0, sizeof(*arena));
}

inline void *arena_push(Arena *arena, uint64_t size, uint64_t alignment, Error *err, bool zero = true) {
    if (!alignment || (alignment & (alignment - 1)) != 0) {
        fail(err, "invalid arena alignment");
        return nullptr;
    }
    const uint64_t at = align_up(arena->used, alignment);
    if (at > arena->reserved || size > arena->reserved - at) {
        fail(err, "arena exhausted (requested=%llu used=%llu reserved=%llu)",
             static_cast<unsigned long long>(size), static_cast<unsigned long long>(arena->used),
             static_cast<unsigned long long>(arena->reserved));
        return nullptr;
    }
    const uint64_t needed = at + size;
    if (needed > arena->committed) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const uint64_t commit_to = align_up(needed, si.dwPageSize);
        const uint64_t amount = commit_to - arena->committed;
        void *p = VirtualAlloc(arena->base + arena->committed, static_cast<SIZE_T>(amount), MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            fail(err, "VirtualAlloc commit failed for %llu bytes (win32=%lu)",
                 static_cast<unsigned long long>(amount), GetLastError());
            return nullptr;
        }
        arena->committed = commit_to;
    }
    void *result = arena->base + at;
    arena->used = needed;
    if (zero && size)
        memset(result, 0, static_cast<size_t>(size));
    return result;
}

template <typename T>
inline T *arena_array(Arena *arena, uint64_t count, Error *err, bool zero = true) {
    if (count > (~0ull) / sizeof(T)) {
        fail(err, "arena array size overflow");
        return nullptr;
    }
    return static_cast<T *>(arena_push(arena, count * sizeof(T), alignof(T), err, zero));
}

struct ArenaMark { uint64_t used; };
inline ArenaMark arena_mark(Arena *a) { return {a->used}; }
inline void arena_reset(Arena *a, ArenaMark mark) {
    if (mark.used <= a->used)
        a->used = mark.used;
}

template <typename T>
struct Vec {
    T *data;
    uint32_t count;
    uint32_t capacity;
    Arena *arena;
    Error *err;

    bool reserve(uint32_t wanted) {
        if (wanted <= capacity)
            return true;
        uint32_t next = capacity ? capacity : 16;
        while (next < wanted) {
            if (next > 0x7fffffffu) {
                next = wanted;
                break;
            }
            next *= 2;
        }
        T *p = arena_array<T>(arena, next, err, false);
        if (!p)
            return false;
        if (data && count)
            memcpy(p, data, static_cast<size_t>(count) * sizeof(T));
        data = p;
        capacity = next;
        return true;
    }

    bool push(const T &value) {
        if (count == capacity && !reserve(count + 1))
            return false;
        data[count++] = value;
        return true;
    }
};

template <typename T>
inline Vec<T> make_vec(Arena *arena, Error *err, uint32_t initial = 0) {
    Vec<T> v{};
    v.arena = arena;
    v.err = err;
    if (initial)
        v.reserve(initial);
    return v;
}

// Non-recursive heap sort avoids implementation-defined qsort allocations.
template <typename T, typename Less>
inline void heap_sort(T *items, uint32_t count, Less less) {
    if (count < 2)
        return;
    auto sift = [&](uint32_t root, uint32_t end) {
        for (;;) {
            uint32_t child = root * 2 + 1;
            if (child >= end)
                break;
            if (child + 1 < end && less(items[child], items[child + 1]))
                ++child;
            if (!less(items[root], items[child]))
                break;
            T temp = items[root];
            items[root] = items[child];
            items[child] = temp;
            root = child;
        }
    };
    for (uint32_t start = count / 2; start > 0; --start)
        sift(start - 1, count);
    for (uint32_t end = count; end > 1; --end) {
        T temp = items[0];
        items[0] = items[end - 1];
        items[end - 1] = temp;
        sift(0, end - 1);
    }
}

struct Buffer {
    uint8_t *base;
    uint64_t reserved;
    uint64_t committed;
    uint64_t size;
};

inline bool buffer_init(Buffer *b, uint64_t reserve_size, Error *err) {
    memset(b, 0, sizeof(*b));
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    reserve_size = align_up(reserve_size, si.dwAllocationGranularity);
    b->base = static_cast<uint8_t *>(VirtualAlloc(nullptr, static_cast<SIZE_T>(reserve_size), MEM_RESERVE, PAGE_NOACCESS));
    if (!b->base)
        return fail(err, "VirtualAlloc buffer reserve failed for %llu bytes (win32=%lu)",
                    static_cast<unsigned long long>(reserve_size), GetLastError());
    b->reserved = reserve_size;
    return true;
}

inline void buffer_release(Buffer *b) {
    if (b->base)
        VirtualFree(b->base, 0, MEM_RELEASE);
    memset(b, 0, sizeof(*b));
}

inline bool buffer_ensure(Buffer *b, uint64_t needed, Error *err) {
    if (needed > b->reserved)
        return fail(err, "output buffer exhausted (needed=%llu reserved=%llu)",
                    static_cast<unsigned long long>(needed), static_cast<unsigned long long>(b->reserved));
    if (needed <= b->committed)
        return true;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uint64_t commit_to = align_up(needed, si.dwPageSize);
    const uint64_t amount = commit_to - b->committed;
    if (!VirtualAlloc(b->base + b->committed, static_cast<SIZE_T>(amount), MEM_COMMIT, PAGE_READWRITE))
        return fail(err, "VirtualAlloc buffer commit failed for %llu bytes (win32=%lu)",
                    static_cast<unsigned long long>(amount), GetLastError());
    b->committed = commit_to;
    return true;
}

inline uint64_t buffer_append(Buffer *b, const void *src, uint64_t size, Error *err) {
    const uint64_t at = b->size;
    if (size > b->reserved - (at <= b->reserved ? at : b->reserved) || !buffer_ensure(b, at + size, err))
        return ~0ull;
    if (size && src)
        memcpy(b->base + at, src, static_cast<size_t>(size));
    else if (size)
        memset(b->base + at, 0, static_cast<size_t>(size));
    b->size += size;
    return at;
}

inline bool buffer_patch(Buffer *b, uint64_t at, const void *src, uint64_t size, Error *err) {
    if (at > b->size || size > b->size - at)
        return fail(err, "buffer patch outside written range");
    memcpy(b->base + at, src, static_cast<size_t>(size));
    return true;
}

inline uint64_t append_u16(Buffer *b, uint16_t v, Error *e) { return buffer_append(b, &v, 2, e); }
inline uint64_t append_u32(Buffer *b, uint32_t v, Error *e) { return buffer_append(b, &v, 4, e); }
inline uint64_t append_f32(Buffer *b, float v, Error *e) { return buffer_append(b, &v, 4, e); }
inline bool patch_u32(Buffer *b, uint64_t at, uint32_t v, Error *e) { return buffer_patch(b, at, &v, 4, e); }

struct Str {
    const char *data;
    uint32_t size;
};

inline Str str_lit(const char *s) { return {s, static_cast<uint32_t>(strlen(s))}; }
inline Str str_from_c(const char *s) { return s ? Str{s, static_cast<uint32_t>(strlen(s))} : Str{}; }
inline bool str_empty(Str s) { return !s.data || !s.size; }
inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }
inline bool str_eq(Str a, Str b) {
    return a.size == b.size && (!a.size || memcmp(a.data, b.data, a.size) == 0);
}
inline bool str_ieq(Str a, Str b) {
    if (a.size != b.size)
        return false;
    for (uint32_t i = 0; i < a.size; ++i)
        if (ascii_lower(a.data[i]) != ascii_lower(b.data[i]))
            return false;
    return true;
}
inline bool str_ieq_c(Str a, const char *b) { return str_ieq(a, str_lit(b)); }
inline bool str_starts_i(Str s, Str prefix) {
    return s.size >= prefix.size && str_ieq({s.data, prefix.size}, prefix);
}
inline bool str_ends_i(Str s, Str suffix) {
    return s.size >= suffix.size && str_ieq({s.data + s.size - suffix.size, suffix.size}, suffix);
}
inline bool str_contains_i(Str haystack, Str needle) {
    if (!needle.size)
        return true;
    if (needle.size > haystack.size)
        return false;
    for (uint32_t i = 0; i + needle.size <= haystack.size; ++i)
        if (str_ieq({haystack.data + i, needle.size}, needle))
            return true;
    return false;
}
inline Str str_trim(Str s) {
    while (s.size && static_cast<unsigned char>(s.data[0]) <= ' ') {
        ++s.data;
        --s.size;
    }
    while (s.size && static_cast<unsigned char>(s.data[s.size - 1]) <= ' ')
        --s.size;
    return s;
}
inline Str path_basename(Str s) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < s.size; ++i)
        if (s.data[i] == '\\' || s.data[i] == '/')
            at = i + 1;
    return {s.data + at, s.size - at};
}
inline Str path_stem(Str s) {
    s = path_basename(s);
    for (uint32_t i = s.size; i > 0; --i)
        if (s.data[i - 1] == '.')
            return {s.data, i - 1};
    return s;
}

inline bool parse_u64(Str s, uint64_t *out) {
    s = str_trim(s);
    if (!s.size)
        return false;
    uint32_t base = 10, i = 0;
    if (s.size >= 2 && s.data[0] == '0' && (s.data[1] == 'x' || s.data[1] == 'X')) {
        base = 16;
        i = 2;
    }
    uint64_t value = 0;
    bool any = false;
    for (; i < s.size; ++i) {
        const char c = s.data[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10u + static_cast<uint32_t>(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10u + static_cast<uint32_t>(c - 'A');
        else return false;
        if (d >= base || value > (~0ull - d) / base)
            return false;
        value = value * base + d;
        any = true;
    }
    if (!any)
        return false;
    *out = value;
    return true;
}

inline bool parse_i64(Str s, int64_t *out) {
    s = str_trim(s);
    bool neg = false;
    if (s.size && (s.data[0] == '-' || s.data[0] == '+')) {
        neg = s.data[0] == '-';
        ++s.data; --s.size;
    }
    uint64_t v = 0;
    if (!parse_u64(s, &v) || v > (neg ? 0x8000000000000000ull : 0x7fffffffffffffffull))
        return false;
    *out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
    return true;
}

inline bool parse_f64(Str s, double *out) {
    s = str_trim(s);
    if (!s.size)
        return false;
    uint32_t i = 0;
    bool neg = false;
    if (s.data[i] == '-' || s.data[i] == '+') {
        neg = s.data[i++] == '-';
        if (i == s.size) return false;
    }
    double value = 0.0;
    bool any = false;
    while (i < s.size && s.data[i] >= '0' && s.data[i] <= '9') {
        value = value * 10.0 + static_cast<double>(s.data[i++] - '0');
        any = true;
    }
    if (i < s.size && s.data[i] == '.') {
        ++i;
        double place = 0.1;
        while (i < s.size && s.data[i] >= '0' && s.data[i] <= '9') {
            value += static_cast<double>(s.data[i++] - '0') * place;
            place *= 0.1;
            any = true;
        }
    }
    if (!any)
        return false;
    int exponent = 0;
    bool exp_neg = false;
    if (i < s.size && (s.data[i] == 'e' || s.data[i] == 'E')) {
        ++i;
        if (i < s.size && (s.data[i] == '-' || s.data[i] == '+'))
            exp_neg = s.data[i++] == '-';
        bool exp_any = false;
        while (i < s.size && s.data[i] >= '0' && s.data[i] <= '9') {
            exponent = exponent * 10 + (s.data[i++] - '0');
            if (exponent > 400) exponent = 400;
            exp_any = true;
        }
        if (!exp_any) return false;
    }
    if (i != s.size)
        return false;
    if (exponent) {
        const double scale = pow(10.0, exp_neg ? -exponent : exponent);
        value *= scale;
    }
    *out = neg ? -value : value;
    return true;
}

struct MappedFile {
    HANDLE file;
    HANDLE mapping;
    const uint8_t *data;
    uint64_t size;
    const char *path;
};

inline bool map_file(const char *path, MappedFile *out, Error *err) {
    memset(out, 0, sizeof(*out));
    out->file = INVALID_HANDLE_VALUE;
    out->path = path;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return fail(err, "cannot open '%s' (win32=%lu)", path, GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0) {
        CloseHandle(f);
        return fail(err, "cannot stat '%s' (win32=%lu)", path, GetLastError());
    }
    out->file = f;
    out->size = static_cast<uint64_t>(size.QuadPart);
    if (!out->size)
        return true;
    HANDLE mapping = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(f);
        out->file = INVALID_HANDLE_VALUE;
        return fail(err, "cannot map '%s' (win32=%lu)", path, GetLastError());
    }
    const void *data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!data) {
        CloseHandle(mapping); CloseHandle(f);
        out->file = INVALID_HANDLE_VALUE;
        return fail(err, "cannot view '%s' (win32=%lu)", path, GetLastError());
    }
    out->mapping = mapping;
    out->data = static_cast<const uint8_t *>(data);
    return true;
}

inline void unmap_file(MappedFile *f) {
    if (f->data) UnmapViewOfFile(f->data);
    if (f->mapping) CloseHandle(f->mapping);
    if (f->file && f->file != INVALID_HANDLE_VALUE) CloseHandle(f->file);
    memset(f, 0, sizeof(*f));
}

inline bool write_entire_file(const char *path, const void *data, uint64_t size, Error *err) {
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return fail(err, "cannot create '%s' (win32=%lu)", path, GetLastError());
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint64_t left = size;
    bool ok = true;
    while (left) {
        const DWORD ask = left > 0x40000000ull ? 0x40000000u : static_cast<DWORD>(left);
        DWORD wrote = 0;
        if (!WriteFile(f, p, ask, &wrote, nullptr) || wrote != ask) {
            fail(err, "write failed for '%s' (win32=%lu)", path, GetLastError());
            ok = false;
            break;
        }
        p += wrote;
        left -= wrote;
    }
    if (ok && !FlushFileBuffers(f))
        ok = fail(err, "flush failed for '%s' (win32=%lu)", path, GetLastError());
    CloseHandle(f);
    return ok;
}

inline bool file_exists(const char *path) {
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool dir_exists(const char *path) {
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool join_path(char *dst, uint32_t cap, const char *a, Str b) {
    const uint32_t na = static_cast<uint32_t>(strlen(a));
    const bool slash = na && a[na - 1] != '\\' && a[na - 1] != '/';
    if (static_cast<uint64_t>(na) + (slash ? 1u : 0u) + b.size + 1 > cap)
        return false;
    memcpy(dst, a, na);
    uint32_t at = na;
    if (slash) dst[at++] = '\\';
    if (b.size) memcpy(dst + at, b.data, b.size);
    dst[at + b.size] = 0;
    return true;
}

enum class JsonKind : uint8_t { Null, Bool, Number, String, Array, Object };
struct Json {
    JsonKind kind;
    Str key;
    Str string;
    double number;
    bool boolean;
    Json *child;
    Json *next;
};

struct JsonParser {
    const char *at;
    const char *end;
    Arena *arena;
    Error *err;
    const char *path;
};

inline void json_skip_ws(JsonParser *p) {
    while (p->at < p->end && static_cast<unsigned char>(*p->at) <= ' ')
        ++p->at;
}

inline Json *json_node(JsonParser *p, JsonKind kind) {
    Json *n = arena_array<Json>(p->arena, 1, p->err);
    if (n) n->kind = kind;
    return n;
}

inline bool json_hex4(const char *s, uint32_t *value) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10u + static_cast<uint32_t>(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10u + static_cast<uint32_t>(c - 'A');
        else return false;
        v = (v << 4) | d;
    }
    *value = v;
    return true;
}

inline bool json_parse_string(JsonParser *p, Str *out) {
    if (p->at >= p->end || *p->at != '"')
        return fail(p->err, "%s: expected JSON string", p->path);
    ++p->at;
    const char *scan = p->at;
    uint64_t decoded = 0;
    while (scan < p->end && *scan != '"') {
        if (static_cast<unsigned char>(*scan) < 0x20)
            return fail(p->err, "%s: control character in JSON string", p->path);
        if (*scan == '\\') {
            ++scan;
            if (scan >= p->end) return fail(p->err, "%s: truncated JSON escape", p->path);
            if (*scan == 'u') {
                if (p->end - scan < 5) return fail(p->err, "%s: truncated JSON unicode escape", p->path);
                scan += 5;
                decoded += 1; // non-ASCII escapes are replaced with '?'; paths/names here are latin1.
                continue;
            }
        }
        ++scan;
        ++decoded;
    }
    if (scan >= p->end)
        return fail(p->err, "%s: unterminated JSON string", p->path);
    char *dst = arena_array<char>(p->arena, decoded + 1, p->err, false);
    if (!dst) return false;
    uint64_t at = 0;
    while (p->at < scan) {
        char c = *p->at++;
        if (c == '\\') {
            c = *p->at++;
            switch (c) {
                case '"': dst[at++] = '"'; break;
                case '\\': dst[at++] = '\\'; break;
                case '/': dst[at++] = '/'; break;
                case 'b': dst[at++] = '\b'; break;
                case 'f': dst[at++] = '\f'; break;
                case 'n': dst[at++] = '\n'; break;
                case 'r': dst[at++] = '\r'; break;
                case 't': dst[at++] = '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!json_hex4(p->at, &cp)) return fail(p->err, "%s: bad JSON unicode escape", p->path);
                    p->at += 4;
                    dst[at++] = cp <= 0xff ? static_cast<char>(cp) : '?';
                } break;
                default: return fail(p->err, "%s: bad JSON escape", p->path);
            }
        } else {
            dst[at++] = c;
        }
    }
    ++p->at; // quote
    dst[at] = 0;
    *out = {dst, static_cast<uint32_t>(at)};
    return true;
}

inline Json *json_parse_value(JsonParser *p);

inline Json *json_parse_container(JsonParser *p, bool object) {
    Json *root = json_node(p, object ? JsonKind::Object : JsonKind::Array);
    if (!root) return nullptr;
    ++p->at;
    json_skip_ws(p);
    const char close = object ? '}' : ']';
    if (p->at < p->end && *p->at == close) {
        ++p->at;
        return root;
    }
    Json **tail = &root->child;
    for (;;) {
        Str key{};
        if (object) {
            if (!json_parse_string(p, &key)) return nullptr;
            json_skip_ws(p);
            if (p->at >= p->end || *p->at != ':') {
                fail(p->err, "%s: expected ':'", p->path);
                return nullptr;
            }
            ++p->at;
            json_skip_ws(p);
        }
        Json *v = json_parse_value(p);
        if (!v) return nullptr;
        v->key = key;
        *tail = v;
        tail = &v->next;
        json_skip_ws(p);
        if (p->at >= p->end) {
            fail(p->err, "%s: unterminated JSON container", p->path);
            return nullptr;
        }
        if (*p->at == close) {
            ++p->at;
            return root;
        }
        if (*p->at != ',') {
            fail(p->err, "%s: expected ','", p->path);
            return nullptr;
        }
        ++p->at;
        json_skip_ws(p);
    }
}

inline Json *json_parse_value(JsonParser *p) {
    json_skip_ws(p);
    if (p->at >= p->end) {
        fail(p->err, "%s: unexpected end of JSON", p->path);
        return nullptr;
    }
    if (*p->at == '{') return json_parse_container(p, true);
    if (*p->at == '[') return json_parse_container(p, false);
    if (*p->at == '"') {
        Json *n = json_node(p, JsonKind::String);
        if (!n || !json_parse_string(p, &n->string)) return nullptr;
        return n;
    }
    const char *start = p->at;
    if (p->end - p->at >= 4 && memcmp(p->at, "null", 4) == 0) {
        p->at += 4; return json_node(p, JsonKind::Null);
    }
    if (p->end - p->at >= 4 && memcmp(p->at, "true", 4) == 0) {
        p->at += 4; Json *n = json_node(p, JsonKind::Bool); if (n) n->boolean = true; return n;
    }
    if (p->end - p->at >= 5 && memcmp(p->at, "false", 5) == 0) {
        p->at += 5; Json *n = json_node(p, JsonKind::Bool); return n;
    }
    while (p->at < p->end) {
        const char c = *p->at;
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'))
            break;
        ++p->at;
    }
    double number = 0.0;
    if (p->at == start || !parse_f64({start, static_cast<uint32_t>(p->at - start)}, &number)) {
        fail(p->err, "%s: invalid JSON token", p->path);
        return nullptr;
    }
    Json *n = json_node(p, JsonKind::Number);
    if (n) n->number = number;
    return n;
}

inline Json *json_parse(MappedFile *file, Arena *arena, Error *err) {
    JsonParser p{reinterpret_cast<const char *>(file->data), reinterpret_cast<const char *>(file->data + file->size),
                 arena, err, file->path ? file->path : "<json>"};
    Json *root = json_parse_value(&p);
    if (!root) return nullptr;
    json_skip_ws(&p);
    if (p.at != p.end) {
        fail(err, "%s: trailing JSON data", p.path);
        return nullptr;
    }
    return root;
}

inline Json *json_get(Json *object, const char *key) {
    if (!object || object->kind != JsonKind::Object) return nullptr;
    const Str k = str_lit(key);
    for (Json *it = object->child; it; it = it->next)
        if (str_eq(it->key, k)) return it;
    return nullptr;
}
inline uint32_t json_count(Json *array) {
    uint32_t n = 0;
    if (array && array->kind == JsonKind::Array)
        for (Json *it = array->child; it; it = it->next) ++n;
    return n;
}
inline double json_number(Json *object, const char *key, double fallback) {
    Json *n = json_get(object, key);
    return n && n->kind == JsonKind::Number ? n->number : fallback;
}
inline int64_t json_integer(Json *object, const char *key, int64_t fallback) {
    Json *n = json_get(object, key);
    if (!n) return fallback;
    if (n->kind == JsonKind::Number) return static_cast<int64_t>(n->number);
    if (n->kind == JsonKind::String) { int64_t v = 0; if (parse_i64(n->string, &v)) return v; }
    if (n->kind == JsonKind::Bool) return n->boolean ? 1 : 0;
    return fallback;
}
inline bool json_boolean(Json *object, const char *key, bool fallback) {
    Json *n = json_get(object, key);
    if (!n) return fallback;
    if (n->kind == JsonKind::Bool) return n->boolean;
    if (n->kind == JsonKind::Number) return n->number != 0.0;
    if (n->kind == JsonKind::String) {
        if (str_ieq_c(n->string, "true") || str_ieq_c(n->string, "yes") || str_ieq_c(n->string, "on") || str_ieq_c(n->string, "1")) return true;
        if (str_ieq_c(n->string, "false") || str_ieq_c(n->string, "no") || str_ieq_c(n->string, "off") || str_ieq_c(n->string, "0") || !n->string.size) return false;
    }
    return fallback;
}
inline Str json_string(Json *object, const char *key) {
    Json *n = json_get(object, key);
    return n && n->kind == JsonKind::String ? n->string : Str{};
}
inline bool json_floats(Json *value, float *dst, uint32_t count) {
    if (!value || value->kind != JsonKind::Array || json_count(value) != count) return false;
    uint32_t i = 0;
    for (Json *it = value->child; it; it = it->next, ++i) {
        if (it->kind != JsonKind::Number) return false;
        dst[i] = static_cast<float>(it->number);
    }
    return true;
}

inline bool append_padded_cstr(Buffer *b, Str s, Error *err, uint32_t alignment = 4) {
    if (buffer_append(b, s.data, s.size, err) == ~0ull || buffer_append(b, nullptr, 1, err) == ~0ull)
        return false;
    const uint64_t string_size = static_cast<uint64_t>(s.size) + 1;
    const uint64_t pad = (alignment - (string_size & (alignment - 1))) & (alignment - 1);
    return !pad || buffer_append(b, nullptr, pad, err) != ~0ull;
}

} // namespace asura
