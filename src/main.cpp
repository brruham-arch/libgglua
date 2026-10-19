#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <mutex>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

enum { I8, I16, I32, I64, F32, F64, NT };
struct TInfo { const char* name; int size; bool fp; };
static const TInfo TI[NT] = {{"i8",1,false},{"i16",2,false},{"i32",4,false},{"i64",8,false},{"f32",4,true},{"f64",8,true}};

static lua_State* L = nullptr;
static int memfd = -1;
static std::mutex G, FM;
static std::string outbuf, initMsg, spec = "anon";

struct Hit { uint64_t addr, old; };
static std::vector<Hit> hits;
static int curType = I32;
static const size_t MAXHITS = 3000000;
static const size_t CHUNK = 1 << 20;

/* ---------- util tipe ---------- */
static int getType(lua_State* s, int idx) {
    const char* n = luaL_optstring(s, idx, "i32");
    for (int i = 0; i < NT; i++) if (!strcmp(n, TI[i].name)) return i;
    luaL_error(s, "tipe tidak dikenal: %s (i8 i16 i32 i64 f32 f64)", n);
    return 0;
}

static bool readRaw(uint64_t a, int t, uint64_t& raw) {
    raw = 0;
    return pread64(memfd, &raw, TI[t].size, (off64_t)a) == TI[t].size;
}

static void pushVal(lua_State* s, int t, uint64_t raw) {
    switch (t) {
        case I8: lua_pushinteger(s, (int8_t)raw); break;
        case I16: lua_pushinteger(s, (int16_t)raw); break;
        case I32: lua_pushinteger(s, (int32_t)raw); break;
        case I64: lua_pushinteger(s, (int64_t)raw); break;
        case F32: { float f; memcpy(&f, &raw, 4); lua_pushnumber(s, f); break; }
        default: { double d; memcpy(&d, &raw, 8); lua_pushnumber(s, d); }
    }
}

static uint64_t toRaw(lua_State* s, int idx, int t) {
    uint64_t r = 0;
    switch (t) {
        case I8: { int8_t v = (int8_t)luaL_checkinteger(s, idx); memcpy(&r, &v, 1); break; }
        case I16: { int16_t v = (int16_t)luaL_checkinteger(s, idx); memcpy(&r, &v, 2); break; }
        case I32: { int32_t v = (int32_t)luaL_checkinteger(s, idx); memcpy(&r, &v, 4); break; }
        case I64: { int64_t v = (int64_t)luaL_checkinteger(s, idx); memcpy(&r, &v, 8); break; }
        case F32: { float v = (float)luaL_checknumber(s, idx); memcpy(&r, &v, 4); break; }
        default: { double v = luaL_checknumber(s, idx); memcpy(&r, &v, 8); }
    }
    return r;
}

template <typename T> static int c3(T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }
static int cmpRaw(int t, uint64_t a, uint64_t b) {
    switch (t) {
        case I8: { int8_t x, y; memcpy(&x, &a, 1); memcpy(&y, &b, 1); return c3(x, y); }
        case I16: { int16_t x, y; memcpy(&x, &a, 2); memcpy(&y, &b, 2); return c3(x, y); }
        case I32: { int32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); return c3(x, y); }
        case I64: { int64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); return c3(x, y); }
        case F32: { float x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); return c3(x, y); }
        default: { double x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); return c3(x, y); }
    }
}

/* ---------- kondisi pencarian (nilai atau rentang "a~b") ---------- */
struct Cond { int t; double lo, hi; int64_t ilo, ihi; };

static void parseCond(lua_State* s, int idx, int t, Cond& c) {
    c.t = t;
    double dlo = 0, dhi = 0;
    int64_t ilo = 0, ihi = 0;
    if (lua_type(s, idx) == LUA_TSTRING) {
        std::string v = lua_tostring(s, idx);
        size_t p = v.find('~');
        std::string a = (p == std::string::npos) ? v : v.substr(0, p);
        std::string b = (p == std::string::npos) ? v : v.substr(p + 1);
        dlo = strtod(a.c_str(), nullptr); dhi = strtod(b.c_str(), nullptr);
        ilo = strtoll(a.c_str(), nullptr, 10); ihi = strtoll(b.c_str(), nullptr, 10);
    } else if (lua_isinteger(s, idx)) {
        ilo = ihi = lua_tointeger(s, idx);
        dlo = dhi = (double)ilo;
    } else {
        dlo = dhi = luaL_checknumber(s, idx);
        ilo = ihi = (int64_t)dlo;
    }
    if (t == F32 && dlo == dhi) dlo = dhi = (double)(float)dlo;
    c.lo = dlo; c.hi = dhi; c.ilo = ilo; c.ihi = ihi;
}

static inline bool matchRaw(const Cond& c, const uint8_t* p) {
    switch (c.t) {
        case I8: { int8_t v; memcpy(&v, p, 1); return v >= c.ilo && v <= c.ihi; }
        case I16: { int16_t v; memcpy(&v, p, 2); return v >= c.ilo && v <= c.ihi; }
        case I32: { int32_t v; memcpy(&v, p, 4); return v >= c.ilo && v <= c.ihi; }
        case I64: { int64_t v; memcpy(&v, p, 8); return v >= c.ilo && v <= c.ihi; }
        case F32: { float v; memcpy(&v, p, 4); return (double)v >= c.lo && (double)v <= c.hi; }
        default: { double v; memcpy(&v, p, 8); return v >= c.lo && v <= c.hi; }
    }
}

/* ---------- maps ---------- */
struct Region { uint64_t s, e; std::string perms, path; };

static std::vector<Region> readMaps() {
    std::vector<Region> v;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return v;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        char perms[8];
        int off = 0;
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &a, &b, perms, &off) < 3) continue;
        std::string path = off > 0 ? std::string(line + off) : std::string();
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        v.push_back({(uint64_t)a, (uint64_t)b, perms, path});
    }
    fclose(f);
    return v;
}

static bool wanted(const Region& r) {
    if (r.perms[0] != 'r' || r.perms[1] != 'w') return false;
    const std::string& p = r.path;
    if (p.find("/dev/") != std::string::npos) return false;
    if (p.compare(0, 5, "[vvar") == 0 || p.compare(0, 5, "[vdso") == 0) return false;
    bool anon = p.empty() || p[0] == '[';
    if (spec == "all") return true;
    if (spec == "anon") return anon;
    if (spec == "native") return anon && p.find("dalvik") == std::string::npos;
    return !anon && p.find(spec) != std::string::npos;
}

/* ---------- API Lua ---------- */
static int l_print(lua_State* s) {
    int n = lua_gettop(s);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char* p = luaL_tolstring(s, i, &len);
        if (i > 1) outbuf += '\t';
        outbuf.append(p, len);
        lua_pop(s, 1);
    }
    outbuf += '\n';
    return 0;
}

static int l_ranges(lua_State* s) {
    std::string v = luaL_optstring(s, 1, spec.c_str());
    spec = v;
    size_t n = 0, bytes = 0;
    for (auto& r : readMaps()) if (wanted(r)) { n++; bytes += r.e - r.s; }
    lua_pushinteger(s, (lua_Integer)n);
    lua_pushnumber(s, bytes / 1048576.0);
    return 2;
}

static int l_maps(lua_State* s) {
    const char* f = luaL_optstring(s, 1, "");
    lua_newtable(s);
    int i = 1;
    for (auto& r : readMaps()) {
        if (*f && r.path.find(f) == std::string::npos) continue;
        char b[1200];
        snprintf(b, sizeof b, "%llx-%llx %s %s", (unsigned long long)r.s, (unsigned long long)r.e, r.perms.c_str(), r.path.c_str());
        lua_pushstring(s, b);
        lua_rawseti(s, -2, i++);
    }
    return 1;
}

static int l_base(lua_State* s) {
    const char* name = luaL_checkstring(s, 1);
    for (auto& r : readMaps()) {
        if (r.path.find(name) != std::string::npos) { lua_pushinteger(s, (lua_Integer)r.s); return 1; }
    }
    lua_pushnil(s);
    return 1;
}

static int l_search(lua_State* s) {
    int t = getType(s, 2);
    Cond c;
    parseCond(s, 1, t, c);
    hits.clear();
    curType = t;
    int sz = TI[t].size;
    bool trunc = false;
    std::vector<uint8_t> buf(CHUNK);
    uint64_t b0 = (uint64_t)buf.data(), b1 = b0 + buf.size();
    for (auto& r : readMaps()) {
        if (!wanted(r)) continue;
        uint64_t a = (r.s + sz - 1) / sz * sz;
        while (a + sz <= r.e && !trunc) {
            size_t want = (size_t)std::min<uint64_t>(CHUNK, r.e - a);
            want -= want % sz;
            if (!want) break;
            ssize_t n = pread64(memfd, buf.data(), want, (off64_t)a);
            size_t adv = want;
            if (n > 0) {
                size_t usable = (size_t)n - ((size_t)n % sz);
                for (size_t o = 0; o < usable; o += sz) {
                    if (!matchRaw(c, &buf[o])) continue;
                    uint64_t ad = a + o;
                    if (ad >= b0 && ad < b1) continue;
                    if (hits.size() >= MAXHITS) { trunc = true; break; }
                    Hit h; h.addr = ad; h.old = 0;
                    memcpy(&h.old, &buf[o], sz);
                    hits.push_back(h);
                }
                if (usable) adv = usable;
            }
            a += adv;
        }
        if (trunc) break;
    }
    if (trunc) outbuf += "[peringatan] hasil dipotong di 3000000, persempit nilai/ranges\n";
    lua_pushinteger(s, (lua_Integer)hits.size());
    return 1;
}

// mode: 0 nilai/rentang, 1 changed, 2 unchanged, 3 increased, 4 decreased
static int doRefine(lua_State* s, int mode, const Cond* c) {
    int t = curType;
    size_t k = 0;
    for (size_t i = 0; i < hits.size(); i++) {
        Hit h = hits[i];
        uint64_t cur = 0;
        if (pread64(memfd, &cur, TI[t].size, (off64_t)h.addr) != TI[t].size) continue;
        bool keep = false;
        switch (mode) {
            case 0: keep = matchRaw(*c, (const uint8_t*)&cur); break;
            case 1: keep = cur != h.old; break;
            case 2: keep = cur == h.old; break;
            case 3: keep = cmpRaw(t, cur, h.old) > 0; break;
            case 4: keep = cmpRaw(t, cur, h.old) < 0; break;
        }
        if (keep) { h.old = cur; hits[k++] = h; }
    }
    hits.resize(k);
    lua_pushinteger(s, (lua_Integer)k);
    return 1;
}

static int l_refine(lua_State* s) { Cond c; parseCond(s, 1, curType, c); return doRefine(s, 0, &c); }
static int l_changed(lua_State* s) { return doRefine(s, 1, nullptr); }
static int l_unchanged(lua_State* s) { return doRefine(s, 2, nullptr); }
static int l_increased(lua_State* s) { return doRefine(s, 3, nullptr); }
static int l_decreased(lua_State* s) { return doRefine(s, 4, nullptr); }
static int l_count(lua_State* s) { lua_pushinteger(s, (lua_Integer)hits.size()); return 1; }
static int l_clear(lua_State* s) { hits.clear(); hits.shrink_to_fit(); return 0; }

static int l_results(lua_State* s) {
    size_t n = (size_t)luaL_optinteger(s, 1, 20);
    size_t off = (size_t)luaL_optinteger(s, 2, 0);
    lua_newtable(s);
    int idx = 1;
    for (size_t i = off; i < hits.size() && i < off + n; i++) {
        uint64_t cur;
        lua_newtable(s);
        lua_pushinteger(s, (lua_Integer)hits[i].addr);
        lua_setfield(s, -2, "addr");
        if (readRaw(hits[i].addr, curType, cur)) pushVal(s, curType, cur); else lua_pushnil(s);
        lua_setfield(s, -2, "value");
        lua_rawseti(s, -2, idx++);
    }
    return 1;
}

static int l_read(lua_State* s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    int t = getType(s, 2);
    uint64_t raw;
    if (!readRaw(a, t, raw)) { lua_pushnil(s); return 1; }
    pushVal(s, t, raw);
    return 1;
}

static int l_write(lua_State* s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    int t = getType(s, 3);
    uint64_t raw = toRaw(s, 2, t);
    lua_pushboolean(s, pwrite64(memfd, &raw, TI[t].size, (off64_t)a) == TI[t].size);
    return 1;
}

static int l_editAll(lua_State* s) {
    uint64_t raw = toRaw(s, 1, curType);
    size_t ok = 0;
    for (auto& h : hits)
        if (pwrite64(memfd, &raw, TI[curType].size, (off64_t)h.addr) == TI[curType].size) { h.old = raw; ok++; }
    lua_pushinteger(s, (lua_Integer)ok);
    return 1;
}

/* ---------- freeze ---------- */
struct Fz { uint64_t addr; int t; uint64_t raw; };
static std::vector<Fz> fz;
static bool fzStarted = false;

static void* freezer(void*) {
    for (;;) {
        usleep(30000);
        std::lock_guard<std::mutex> g(FM);
        for (auto& f : fz) pwrite64(memfd, &f.raw, TI[f.t].size, (off64_t)f.addr);
    }
    return nullptr;
}

static void addFreeze(uint64_t a, int t, uint64_t raw) {
    std::lock_guard<std::mutex> g(FM);
    for (auto& f : fz) if (f.addr == a) { f.t = t; f.raw = raw; return; }
    fz.push_back({a, t, raw});
    if (!fzStarted) {
        fzStarted = true;
        pthread_t th;
        pthread_create(&th, nullptr, freezer, nullptr);
        pthread_detach(th);
    }
}

static int l_freeze(lua_State* s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    int t = getType(s, 3);
    addFreeze(a, t, toRaw(s, 2, t));
    return 0;
}

static int l_freezeAll(lua_State* s) {
    uint64_t raw = toRaw(s, 1, curType);
    for (auto& h : hits) addFreeze(h.addr, curType, raw);
    lua_pushinteger(s, (lua_Integer)hits.size());
    return 1;
}

static int l_unfreeze(lua_State* s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    std::lock_guard<std::mutex> g(FM);
    for (size_t i = 0; i < fz.size(); i++) if (fz[i].addr == a) { fz.erase(fz.begin() + i); break; }
    return 0;
}

static int l_unfreezeAll(lua_State*) { std::lock_guard<std::mutex> g(FM); fz.clear(); return 0; }
static int l_frozen(lua_State* s) { std::lock_guard<std::mutex> g(FM); lua_pushinteger(s, (lua_Integer)fz.size()); return 1; }

/* ---------- init & run ---------- */
static const char* PRELUDE = R"LUA(
function gg.show(n, off)
  off = off or 0
  local r = gg.results(n or 20, off)
  for i, h in ipairs(r) do
    print(string.format("%d. 0x%x = %s", i + off, h.addr, tostring(h.value)))
  end
  print("total: " .. gg.count())
end
function gg.load(name) return dofile(gg.dir .. "/" .. name) end
function gg.hex(n) return string.format("0x%x", n) end
)LUA";

static void doInit(const std::string& dir) {
    if (L) return;
    memfd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    static const luaL_Reg fns[] = {
        {"ranges", l_ranges}, {"maps", l_maps}, {"base", l_base},
        {"search", l_search}, {"refine", l_refine},
        {"changed", l_changed}, {"unchanged", l_unchanged},
        {"increased", l_increased}, {"decreased", l_decreased},
        {"count", l_count}, {"clear", l_clear}, {"results", l_results},
        {"read", l_read}, {"write", l_write}, {"editAll", l_editAll},
        {"freeze", l_freeze}, {"freezeAll", l_freezeAll},
        {"unfreeze", l_unfreeze}, {"unfreezeAll", l_unfreezeAll}, {"frozen", l_frozen},
        {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_pushstring(L, dir.c_str());
    lua_setfield(L, -2, "dir");
    lua_setglobal(L, "gg");
    std::string pp = "package.path=package.path..';" + dir + "/?.lua'";
    luaL_dostring(L, pp.c_str());
    luaL_dostring(L, PRELUDE);
    lua_settop(L, 0);
    std::string a = dir + "/autoexec.lua";
    if (access(a.c_str(), R_OK) == 0) {
        outbuf.clear();
        if (luaL_dofile(L, a.c_str())) {
            initMsg = std::string("autoexec error: ") + lua_tostring(L, -1) + "\n";
        } else initMsg = outbuf;
        lua_settop(L, 0);
    }
}

static std::string runLua(const std::string& code) {
    std::lock_guard<std::mutex> g(G);
    if (!L) return "belum init\n";
    outbuf = initMsg;
    initMsg.clear();
    int top = lua_gettop(L);
    std::string ex = "return " + code;
    int st = luaL_loadstring(L, ex.c_str());
    if (st != LUA_OK) { lua_settop(L, top); st = luaL_loadstring(L, code.c_str()); }
    if (st == LUA_OK) st = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (st != LUA_OK) {
        const char* m = lua_tostring(L, -1);
        outbuf += "error: ";
        outbuf += m ? m : "?";
        outbuf += '\n';
    } else {
        int n = lua_gettop(L) - top;
        if (n > 0) {
            for (int i = 1; i <= n; i++) {
                size_t len;
                const char* p = luaL_tolstring(L, top + i, &len);
                if (i > 1) outbuf += '\t';
                outbuf.append(p, len);
                lua_pop(L, 1);
            }
            outbuf += '\n';
        }
    }
    lua_settop(L, top);
    return outbuf;
}

extern "C" JNIEXPORT void JNICALL Java_com_gglua_GG_nativeInit(JNIEnv* e, jclass, jstring d) {
    const char* c = e->GetStringUTFChars(d, nullptr);
    std::string dir = c;
    e->ReleaseStringUTFChars(d, c);
    std::lock_guard<std::mutex> g(G);
    doInit(dir);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_gglua_GG_nativeRun(JNIEnv* e, jclass, jstring j) {
    const char* c = e->GetStringUTFChars(j, nullptr);
    std::string code = c;
    e->ReleaseStringUTFChars(j, c);
    std::string r = runLua(code);
    for (auto& ch : r) if ((unsigned char)ch >= 0x80) ch = '?';
    return e->NewStringUTF(r.c_str());
}
