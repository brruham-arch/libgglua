#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <type_traits>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

static std::string DIR;
static lua_State* L = nullptr;
static int memfd = -1;

static void logm(const std::string& m) {
    __android_log_write(ANDROID_LOG_INFO, "gglua", m.c_str());
    FILE* f = fopen((DIR + "/gg_log.txt").c_str(), "a");
    if (f) { fprintf(f, "%s\n", m.c_str()); fclose(f); }
}

static int l_log(lua_State* s) { logm(luaL_checkstring(s, 1)); return 0; }

static int l_base(lua_State* s) {
    const char* name = luaL_checkstring(s, 1);
    FILE* f = fopen("/proc/self/maps", "r");
    char line[768];
    while (f && fgets(line, sizeof line, f)) {
        if (strstr(line, name)) {
            lua_pushinteger(s, (lua_Integer)strtoull(line, nullptr, 16));
            fclose(f);
            return 1;
        }
    }
    if (f) fclose(f);
    lua_pushnil(s);
    return 1;
}

template <typename T> static int rd(lua_State* s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    T v;
    if (pread64(memfd, &v, sizeof v, (off64_t)a) != (ssize_t)sizeof v) { lua_pushnil(s); return 1; }
    if constexpr (std::is_floating_point<T>::value) lua_pushnumber(s, (lua_Number)v);
    else lua_pushinteger(s, (lua_Integer)v);
    return 1;
}

template <typename T> static int wr(lua_State* s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    T v;
    if constexpr (std::is_floating_point<T>::value) v = (T)luaL_checknumber(s, 2);
    else v = (T)luaL_checkinteger(s, 2);
    lua_pushboolean(s, pwrite64(memfd, &v, sizeof v, (off64_t)a) == (ssize_t)sizeof v);
    return 1;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void utf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
}

static bool pstr(const char*& p, std::string& out) {
    if (*p != '"') return false;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': out += '\n'; p++; break;
                case 't': out += '\t'; p++; break;
                case 'r': out += '\r'; p++; break;
                case 'b': out += '\b'; p++; break;
                case 'f': out += '\f'; p++; break;
                case 'u': {
                    uint32_t c = 0;
                    for (int i = 1; i <= 4; i++) c = c * 16 + hexv(p[i]);
                    p += 5;
                    if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                        uint32_t d = 0;
                        for (int i = 2; i <= 5; i++) d = d * 16 + hexv(p[i]);
                        p += 6;
                        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                    }
                    utf8(out, c);
                    break;
                }
                default: if (*p) { out += *p; p++; }
            }
        } else out += *p++;
    }
    if (*p == '"') p++;
    return true;
}

// JSON datar (satu baris) -> tabel Lua di puncak stack
static bool pushEvent(lua_State* s, const std::string& line, std::string& type) {
    const char* p = line.c_str();
    while (*p && *p != '{') p++;
    if (!*p) return false;
    p++;
    lua_newtable(s);
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n') p++;
        if (*p == '}' || !*p) break;
        std::string k;
        if (!pstr(p, k)) break;
        while (*p == ' ' || *p == ':') p++;
        bool have = true;
        if (*p == '"') {
            std::string v;
            pstr(p, v);
            lua_pushlstring(s, v.data(), v.size());
            if (k == "type") type = v;
        } else {
            const char* st = p;
            while (*p && *p != ',' && *p != '}') p++;
            std::string t(st, p - st);
            while (!t.empty() && (t.back() == ' ' || t.back() == '\r')) t.pop_back();
            if (t == "null" || t.empty()) have = false;
            else if (t == "true") lua_pushboolean(s, 1);
            else if (t == "false") lua_pushboolean(s, 0);
            else if (t.find_first_of(".eE") == std::string::npos) lua_pushinteger(s, (lua_Integer)strtoll(t.c_str(), nullptr, 10));
            else lua_pushnumber(s, strtod(t.c_str(), nullptr));
        }
        if (have) lua_setfield(s, -2, k.c_str());
    }
    return true;
}

static void dispatch(const std::string& line) {
    if (line.empty()) return;
    std::string type;
    int top = lua_gettop(L);
    if (!pushEvent(L, line, type) || type.empty()) { lua_settop(L, top); return; }
    std::string fn = "on";
    fn += (char)toupper((unsigned char)type[0]);
    fn += type.substr(1);
    lua_getglobal(L, fn.c_str());
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        if (lua_pcall(L, 1, 0, 0)) logm("ERR " + fn + ": " + lua_tostring(L, -1));
    }
    lua_settop(L, top);
}

static void* worker(void*) {
    char cmd[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) { read(fd, cmd, sizeof cmd - 1); close(fd); }
    std::string pkg(cmd);
    size_t c = pkg.find(':');
    if (c != std::string::npos) pkg = pkg.substr(0, c);
    std::string media = "/storage/emulated/0/Android/media/" + pkg;
    DIR = media + "/gg";
    mkdir(media.c_str(), 0777);
    mkdir(DIR.c_str(), 0777);
    sleep(2);

    memfd = open("/proc/self/mem", O_RDWR);
    L = luaL_newstate();
    luaL_openlibs(L);
    static const luaL_Reg fns[] = {
        {"log", l_log}, {"base", l_base},
        {"readI32", rd<int32_t>}, {"readI64", rd<int64_t>},
        {"readF32", rd<float>}, {"readF64", rd<double>},
        {"writeI32", wr<int32_t>}, {"writeI64", wr<int64_t>},
        {"writeF32", wr<float>}, {"writeF64", wr<double>},
        {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "gg");

    logm("gglua start, pkg=" + pkg + " memfd=" + std::to_string(memfd));
    std::string script = DIR + "/main.lua";
    if (luaL_dofile(L, script.c_str())) {
        logm(std::string("ERR main.lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
    } else logm("main.lua OK");

    std::string evf = DIR + "/tiktok_events.json";
    struct stat st;
    long off = (stat(evf.c_str(), &st) == 0) ? (long)st.st_size : 0;
    std::string buf;
    for (;;) {
        usleep(200000);
        if (stat(evf.c_str(), &st) != 0) continue;
        if (st.st_size < off) off = 0;
        if (st.st_size == off) continue;
        FILE* f = fopen(evf.c_str(), "rb");
        if (!f) continue;
        fseek(f, off, SEEK_SET);
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) { buf.append(tmp, n); off += (long)n; }
        fclose(f);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            dispatch(line);
        }
    }
    return nullptr;
}

__attribute__((constructor)) static void init() {
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_detach(t);
}
