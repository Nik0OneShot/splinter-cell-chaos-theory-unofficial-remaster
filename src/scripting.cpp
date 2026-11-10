// Lua scripting (mod support, phase 3).
//
// Every enabled mod's Scripts\*.lua files run in one Lua 5.4 state, in mod load order (lowest priority first) and
// file-name order. Each mod has its own global table (its scripts share it; other mods can't see it, except through
// the 'shared' table). The API is documented for modders in Mods\SCRIPTING.txt; in short:
//   events:   on("map"|"level"|"tick", fn)       timers: after(s, fn), every(s, fn), run(fn) + wait(s), cancel(t)
//   game:     player(), level(), map(), time(), actors(class), objects(class), class(name)
//   objects:  obj.Property / obj.Property = v (any UnrealScript property, by name), obj:Function(args...)
//             valid(obj), isa(obj, class), nameof(obj), classof(obj), fullname(obj), dump(obj)
//   input:    key(name) (held), pressed(name) (this frame)      misc: print, vec(x, y, z), shared
// The sandbox has no io, os.execute, debug, package or binary chunks: a script can't touch files or run programs.
// A call that runs for more than a second (an endless loop) is stopped with an error. Ctrl+F11 (ReloadScriptsKey)
// reloads every script.
//
// Engine layout (splintercell3.exe, found from execGetPropertyText 0x10CF81D0, its FindField 0x109E41B0 and the
// field iterator 0x10919A90, UIntProperty::ExportText 0x10CEA060, the property classes' ExportTextItem and
// UObject::ProcessEvent 0x10CF4A90):
//   UObject  Index +04h, Name +20h (FName), Class +24h
//   UField   SuperField +28h, Next +2Ch          UStruct  Children +48h
//   UProperty ArrayDim +34h (word), ElementSize +36h (word), PropertyFlags +38h (from UProperty::Serialize 0x10CEA000;
//             here CPF_Parm 20h, CPF_OptionalParm 10h, CPF_OutParm 40h, CPF_ReturnParm 100h, unlike stock UE2),
//             Offset +44h (word); BoolProperty BitMask +54h,
//             Object/ClassProperty PropertyClass +54h (ClassProperty MetaClass +58h), StructProperty Struct +54h,
//             ArrayProperty Inner +54h; the type is the property's class name ("IntProperty", ...)
//   UFunction FunctionFlags +6Ch (FUNC_Native 400h), iNative +70h (word), ParmsSize +74h (word), ReturnValueOffset
//             +76h (word), Func +78h. Parameters are the function's own fields with CPF_Parm.
//   GObjObjects (TArray<UObject*>) from TObjectIterator::operator++; FString::operator=(const TCHAR*) (two identical
//   copies) allocates through GMalloc, so strings handed to the engine are the engine's own.
// Script functions are called through ProcessEvent (vtable +10h). Natives read their arguments as bytecode, so they
// get them as constant tokens, or (strings, structs, out parameters) as EX_LocalVariable (00h + UProperty*) pointing
// into a locals buffer we fill: the VM's execLocalVariable (GNatives[0], 0x10CEE960) reads Locals + Offset and deep
// copies it with CopyCompleteValue (vtable +9Ch), and an out parameter's native writes back into the buffer.
// Calls return the return value first, then every out parameter.
// Every engine read and write goes through SEH-guarded copies, and objects are checked against GObjObjects (index,
// name) before each use, so a script holding a destroyed actor gets an error instead of a crash.
#include "pch.h"
#include "features.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

namespace
{
    constexpr uintptr_t kObjIndex = 0x04, kObjName = 0x20, kObjClass = 0x24;
    constexpr uintptr_t kFieldSuper = 0x28, kFieldNext = 0x2C, kStructChildren = 0x48;
    constexpr uintptr_t kPropDim = 0x34, kPropSize = 0x36, kPropFlags = 0x38, kPropOffset = 0x44, kPropRef = 0x54, kPropRef2 = 0x58;
    // Parameter flags differ from stock UE2 (80h/100h/400h). From Engine.u/Core.u: GetAxes's in parameter A 20h, its out
    // parameter X 60h, SetLocation's optional NoCheck 30h, a ReturnValue 160h.
    constexpr uint32_t CPF_Parm = 0x20, CPF_OutParm = 0x40, CPF_ReturnParm = 0x100;
    constexpr uintptr_t kFuncFlags = 0x6C, kFuncNative = 0x70, kFuncParms = 0x74, kFuncReturn = 0x76, kFuncCode = 0x78;
    constexpr uint32_t kFuncNativeFlag = 0x400;
    constexpr uintptr_t kActorFlags = 0x28, kActorLevel = 0x78, kActorXLevel = 0x7C, kLevelTime = 0x430;
    constexpr uint32_t kDeleteMe = 4;
    constexpr uintptr_t kXLevelActors = 0x28, kXLevelCount = 0x2C;

    using StrAssign_t = void*(__fastcall*)(void* str, void* edx, const wchar_t* text);
    using ProcessEvent_t = void(__fastcall*)(void* obj, void* edx, void* func, void* parms, void* result);

    void*** g_objData = nullptr;    // &GObjObjects.Data
    int* g_objNum = nullptr;        // &GObjObjects.Num
    StrAssign_t pStrAssign = nullptr;

    lua_State* L = nullptr;
    void* g_pawn = nullptr;
    std::wstring g_map;
    bool g_levelPending = false;
    bool g_levelDue = false;        // a mission just started: apply the mods' level changes at the next "level"
    bool g_levelDumpHeld = false;
    int g_depth = 0;                // nesting of calls into Lua (only the outermost sets the time limit)
    LARGE_INTEGER g_deadline{}, g_freq{};
    BYTE g_keys[256] = {}, g_prevKeys[256] = {};
    bool g_reloadHeld = false;
    int g_scriptCount = 0;

    // --- Guarded engine access (no C++ objects in these, so SEH is allowed) -------------------------------

    bool SafeRead(const void* src, void* dst, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SafeWrite(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    template <class T> T Rd(const void* p, uintptr_t off, T def = T{})
    {
        T v;
        return p && SafeRead(reinterpret_cast<const uint8_t*>(p) + off, &v, sizeof(T)) ? v : def;
    }
    bool StrAssign(void* str, const wchar_t* text)
    {
        if (!pStrAssign) return false;
        __try { pStrAssign(str, nullptr, text); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool CallEvent(void* obj, void* func, void* parms)
    {
        __try
        {
            const auto pe = reinterpret_cast<ProcessEvent_t>((*reinterpret_cast<void***>(obj))[0x10 / 4]);
            pe(obj, nullptr, func, parms, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Alive(void* obj)
    {
        if (!obj || !g_objData || !g_objNum) return false;
        const int index = Rd<int>(obj, kObjIndex, -1);
        void** data = Rd<void**>(g_objData, 0);
        const int num = Rd<int>(g_objNum, 0);
        return data && index >= 0 && index < num && Rd<void*>(data, uintptr_t(index) * 4) == obj;
    }
    void* ClassOf(void* obj) { return Rd<void*>(obj, kObjClass); }
    int NameOf(void* obj) { return Rd<int>(obj, kObjName); }
    void* Super(void* field) { return Rd<void*>(field, kFieldSuper); }
    bool IsChildOf(void* cls, void* base)
    {
        for (int i = 0; cls && i < 256; ++i, cls = Super(cls))
            if (cls == base) return true;
        return false;
    }
    bool IsA(void* obj, void* cls) { return IsChildOf(ClassOf(obj), cls); }

    std::string NameStr(int name)
    {
        wchar_t buf[128];
        if (!Script::NameText(name, buf, 128)) return name ? "?" : "None";
        std::string s;
        for (const wchar_t* p = buf; *p; ++p) s += static_cast<char>(*p);
        return s;
    }

    std::wstring Wide(const char* s, size_t n)
    {
        if (!n) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s, int(n), nullptr, 0);
        std::wstring w(size_t(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, int(n), w.data(), len);
        return w;
    }
    std::string Utf8(const wchar_t* s, size_t n)
    {
        if (!n) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, s, int(n), nullptr, 0, nullptr, nullptr);
        std::string u(size_t(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, int(n), u.data(), len, nullptr, nullptr);
        return u;
    }

    void FullNameRaw(void* obj, wchar_t* buf)
    {
        __try { Game::GetFullName(obj, nullptr, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
    }

    std::string FullName(void* obj)
    {
        if (!Game::GetFullName) return NameStr(ClassOf(obj) ? NameOf(ClassOf(obj)) : 0) + " " + NameStr(NameOf(obj));
        wchar_t buf[512] = {};
        FullNameRaw(obj, buf);
        return Utf8(buf, wcslen(buf));
    }

    // --- Properties ------------------------------------------------------------------------------------

    enum class T : uint8_t { None, Byte, Int, Bool, Float, Name, Str, Object, Class, Struct, Array, Other };
    struct Prop
    {
        void* p = nullptr;
        T type = T::None;
        uint16_t offset = 0, size = 0, dim = 1;
        uint32_t mask = 0, flags = 0;
        void* ref = nullptr;    // property class / struct / inner property
        void* ref2 = nullptr;   // ClassProperty meta class
        int name = 0;
        bool vector = false, rotator = false;   // StructProperty of Vector / Rotator
    };
    std::unordered_map<void*, Prop> g_props;                       // UProperty -> description (non-properties: type None)
    std::unordered_map<uint64_t, void*> g_findCache;               // (class, name) -> UProperty (or null)
    std::unordered_map<std::string, int> g_nameCache;              // Lua string -> FName

    bool Is(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

    const Prop& Describe(void* field)
    {
        auto it = g_props.find(field);
        if (it != g_props.end()) return it->second;
        Prop d;
        d.p = field;
        d.name = NameOf(field);
        const std::string cls = NameStr(NameOf(ClassOf(field)));
        if (cls.size() > 8 && _stricmp(cls.c_str() + cls.size() - 8, "Property") == 0)
        {
            d.offset = Rd<uint16_t>(field, kPropOffset);
            d.size = Rd<uint16_t>(field, kPropSize);
            d.dim = std::max<uint16_t>(1, Rd<uint16_t>(field, kPropDim));
            d.ref = Rd<void*>(field, kPropRef);
            d.flags = Rd<uint32_t>(field, kPropFlags);
            if (Is(cls, "ByteProperty")) d.type = T::Byte;
            else if (Is(cls, "IntProperty")) d.type = T::Int;
            else if (Is(cls, "BoolProperty")) { d.type = T::Bool; d.mask = Rd<uint32_t>(field, kPropRef); }
            else if (Is(cls, "FloatProperty")) d.type = T::Float;
            else if (Is(cls, "NameProperty")) d.type = T::Name;
            else if (Is(cls, "StrProperty")) d.type = T::Str;
            else if (Is(cls, "ObjectProperty")) d.type = T::Object;
            else if (Is(cls, "ClassProperty")) { d.type = T::Class; d.ref2 = Rd<void*>(field, kPropRef2); }
            else if (Is(cls, "StructProperty"))
            {
                d.type = T::Struct;
                const std::string sname = NameStr(NameOf(d.ref));
                d.vector = Is(sname, "Vector");
                d.rotator = Is(sname, "Rotator");
            }
            else if (Is(cls, "ArrayProperty")) d.type = T::Array;
            else d.type = T::Other;
        }
        return g_props.emplace(field, d).first->second;
    }

    // Calls fn(field) for every field of 'st' and (when 'inherited') its parents; fn returns false to stop.
    template <class F> void Fields(void* st, bool inherited, F fn)
    {
        int guard = 0;
        for (void* s = st; s && guard < 20000; s = inherited ? Super(s) : nullptr)
            for (void* f = Rd<void*>(s, kStructChildren); f && ++guard < 20000; f = Rd<void*>(f, kFieldNext))
                if (!fn(f)) return;
    }

    void* FindProp(void* cls, int name)
    {
        const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(cls)) << 32) | uint32_t(name);
        auto it = g_findCache.find(key);
        if (it != g_findCache.end()) return it->second;
        void* found = nullptr;
        Fields(cls, true, [&](void* f) {
            if (NameOf(f) == name && Describe(f).type != T::None) { found = f; return false; }
            return true;
        });
        g_findCache[key] = found;
        return found;
    }

    int NameIdx(const char* s)
    {
        auto it = g_nameCache.find(s);
        if (it != g_nameCache.end()) return it->second;
        const int idx = Script::Name(Wide(s, strlen(s)).c_str());
        if (idx) g_nameCache[s] = idx;
        return idx;
    }

    // --- Objects in Lua --------------------------------------------------------------------------------

    const char* const kObjMeta = "SCCT.Object";
    struct LObj { void* p; int index; int name; };

    void PushObject(lua_State* s, void* p)
    {
        if (!p || !Alive(p)) { lua_pushnil(s); return; }
        const int index = Rd<int>(p, kObjIndex), name = NameOf(p);
        lua_getfield(s, LUA_REGISTRYINDEX, "SCCT.objects");
        lua_rawgetp(s, -1, p);
        if (auto* u = static_cast<LObj*>(luaL_testudata(s, -1, kObjMeta)); u && u->index == index && u->name == name)
        {
            lua_remove(s, -2);
            return;
        }
        lua_pop(s, 1);
        auto* u = static_cast<LObj*>(lua_newuserdatauv(s, sizeof(LObj), 0));
        *u = LObj{ p, index, name };
        luaL_setmetatable(s, kObjMeta);
        lua_pushvalue(s, -1);
        lua_rawsetp(s, -3, p);
        lua_remove(s, -2);
    }

    bool StillSame(const LObj* u)
    {
        return Alive(u->p) && Rd<int>(u->p, kObjIndex) == u->index && NameOf(u->p) == u->name;
    }

    void* CheckObject(lua_State* s, int i)
    {
        auto* u = static_cast<LObj*>(luaL_checkudata(s, i, kObjMeta));
        if (!StillSame(u)) luaL_error(s, "the object no longer exists");
        return u->p;
    }
    void* OptObject(lua_State* s, int i)
    {
        if (lua_isnoneornil(s, i)) return nullptr;
        return CheckObject(s, i);
    }

    void* FindClass(const char* name);

    // A class given as a class object or a class name.
    void* CheckClass(lua_State* s, int i)
    {
        if (lua_type(s, i) == LUA_TSTRING)
        {
            void* c = FindClass(lua_tostring(s, i));
            if (!c) luaL_error(s, "no class named '%s'", lua_tostring(s, i));
            return c;
        }
        return CheckObject(s, i);
    }

    // --- Reading and writing values --------------------------------------------------------------------

    void PushElem(lua_State* s, const Prop& d, const uint8_t* addr, int depth);

    // The canonical field name of a Vector/Rotator member (by its offset), or the engine's name.
    std::string FieldName(const Prop& c, bool vector, bool rotator)
    {
        static const char* const kVec[3] = { "X", "Y", "Z" };
        static const char* const kRot[3] = { "Pitch", "Yaw", "Roll" };
        if ((vector || rotator) && c.offset % 4 == 0 && c.offset < 12)
            return (vector ? kVec : kRot)[c.offset / 4];
        return NameStr(c.name);
    }

    // Pushes t[name] for the table at 'idx', matching the key without case when there's no exact match.
    void GetFieldCI(lua_State* s, int idx, const std::string& name)
    {
        idx = lua_absindex(s, idx);
        lua_getfield(s, idx, name.c_str());
        if (!lua_isnil(s, -1)) return;
        lua_pop(s, 1);
        lua_pushnil(s);
        while (lua_next(s, idx))
        {
            if (lua_type(s, -2) == LUA_TSTRING && _stricmp(lua_tostring(s, -2), name.c_str()) == 0)
            {
                lua_remove(s, -2);   // keep the value
                return;
            }
            lua_pop(s, 1);
        }
        lua_pushnil(s);
    }

    void PushStruct(lua_State* s, void* st, const uint8_t* addr, bool vector, bool rotator, int depth)
    {
        static bool noted = false;
        if (vector && !noted)
        {
            noted = true;
            std::string names;
            Fields(st, true, [&](void* f) { names += (names.empty() ? "" : ", ") + NameStr(NameOf(f)); return true; });
            LOG("[LUA] the engine names the Vector fields: %s (scripts use X, Y, Z)", names.c_str());
        }
        lua_newtable(s);
        Fields(st, true, [&](void* f) {
            const Prop& c = Describe(f);
            if (c.type == T::None) return true;
            if (c.dim > 1)
            {
                lua_createtable(s, c.dim, 0);
                for (int i = 0; i < c.dim; ++i) { PushElem(s, c, addr + c.offset + i * c.size, depth + 1); lua_rawseti(s, -2, i + 1); }
            }
            else
                PushElem(s, c, addr + c.offset, depth + 1);
            lua_setfield(s, -2, FieldName(c, vector, rotator).c_str());
            return true;
        });
        lua_getfield(s, LUA_REGISTRYINDEX, vector ? "SCCT.vector" : "SCCT.struct");
        if (lua_istable(s, -1)) lua_setmetatable(s, -2); else lua_pop(s, 1);
    }

    // Pushes one element of property 'd' stored at engine address 'addr'.
    void PushElem(lua_State* s, const Prop& d, const uint8_t* addr, int depth)
    {
        if (depth > 8) { lua_pushnil(s); return; }
        switch (d.type)
        {
        case T::Byte: lua_pushinteger(s, Rd<uint8_t>(addr, 0)); return;
        case T::Int: lua_pushinteger(s, Rd<int32_t>(addr, 0)); return;
        case T::Bool: lua_pushboolean(s, (Rd<uint32_t>(addr, 0) & d.mask) != 0); return;
        case T::Float: lua_pushnumber(s, Rd<float>(addr, 0)); return;
        case T::Name: { const int n = Rd<int>(addr, 0); if (n) lua_pushstring(s, NameStr(n).c_str()); else lua_pushnil(s); return; }
        case T::Str:
        {
            const wchar_t* data = Rd<const wchar_t*>(addr, 0);
            const int num = Rd<int>(addr, 4);
            std::wstring w;
            if (data && num > 1 && num < 65536)
            {
                w.resize(size_t(num - 1));
                if (!SafeRead(data, w.data(), (num - 1) * sizeof(wchar_t))) w.clear();
            }
            const std::string u = Utf8(w.c_str(), wcslen(w.c_str()));
            lua_pushlstring(s, u.data(), u.size());
            return;
        }
        case T::Object: case T::Class: PushObject(s, Rd<void*>(addr, 0)); return;
        case T::Struct: PushStruct(s, d.ref, addr, d.vector, d.rotator, depth); return;
        case T::Array:
        {
            const uint8_t* data = Rd<const uint8_t*>(addr, 0);
            const int num = std::clamp(Rd<int>(addr, 4), 0, 100000);
            const Prop& inner = Describe(d.ref);
            lua_createtable(s, num, 0);
            for (int i = 0; data && i < num; ++i) { PushElem(s, inner, data + i * inner.size, depth + 1); lua_rawseti(s, -2, i + 1); }
            return;
        }
        default: lua_pushnil(s); return;
        }
    }

    // Pushes the value of property 'd' of an object/struct at 'base' (a table for static arrays).
    void PushProp(lua_State* s, const Prop& d, const uint8_t* base)
    {
        if (d.dim > 1)
        {
            lua_createtable(s, d.dim, 0);
            for (int i = 0; i < d.dim; ++i) { PushElem(s, d, base + d.offset + i * d.size, 0); lua_rawseti(s, -2, i + 1); }
        }
        else
            PushElem(s, d, base + d.offset, 0);
    }

    void WriteElem(lua_State* s, int idx, const Prop& d, uint8_t* addr, const char* what);

    void WriteStruct(lua_State* s, int idx, void* st, uint8_t* addr, bool vector, bool rotator, const char* what)
    {
        idx = lua_absindex(s, idx);
        luaL_checktype(s, idx, LUA_TTABLE);
        Fields(st, true, [&](void* f) {
            const Prop& c = Describe(f);
            if (c.type == T::None) return true;
            const std::string n = FieldName(c, vector, rotator);
            GetFieldCI(s, idx, n);
            if (!lua_isnil(s, -1))
            {
                if (c.dim > 1)
                {
                    luaL_checktype(s, -1, LUA_TTABLE);
                    for (int i = 0; i < c.dim; ++i)
                    {
                        lua_rawgeti(s, -1, i + 1);
                        if (!lua_isnil(s, -1)) WriteElem(s, -1, c, addr + c.offset + i * c.size, n.c_str());
                        lua_pop(s, 1);
                    }
                }
                else
                    WriteElem(s, -1, c, addr + c.offset, n.c_str());
            }
            lua_pop(s, 1);
            return true;
        });
    }

    void Store(lua_State* s, void* addr, const void* v, size_t n, const char* what)
    {
        if (!SafeWrite(addr, v, n)) luaL_error(s, "couldn't write '%s'", what);
    }

    // Writes the Lua value at 'idx' into one element of property 'd' at engine address 'addr'.
    void WriteElem(lua_State* s, int idx, const Prop& d, uint8_t* addr, const char* what)
    {
        idx = lua_absindex(s, idx);
        switch (d.type)
        {
        case T::Byte: { const uint8_t v = uint8_t(luaL_checkinteger(s, idx)); Store(s, addr, &v, 1, what); return; }
        case T::Int: { const int32_t v = int32_t(luaL_checkinteger(s, idx)); Store(s, addr, &v, 4, what); return; }
        case T::Float: { const float v = float(luaL_checknumber(s, idx)); Store(s, addr, &v, 4, what); return; }
        case T::Bool:
        {
            uint32_t v = Rd<uint32_t>(addr, 0);
            v = lua_toboolean(s, idx) ? (v | d.mask) : (v & ~d.mask);
            Store(s, addr, &v, 4, what);
            return;
        }
        case T::Name:
        {
            int v = 0;
            if (!lua_isnil(s, idx))
            {
                size_t n = 0;
                const char* str = luaL_checklstring(s, idx, &n);
                v = Script::NameAdd(Wide(str, n).c_str());
            }
            Store(s, addr, &v, 4, what);
            return;
        }
        case T::Str:
        {
            size_t n = 0;
            const char* str = lua_isnil(s, idx) ? "" : luaL_checklstring(s, idx, &n);
            if (!StrAssign(addr, Wide(str, n).c_str())) luaL_error(s, "couldn't write '%s'", what);
            return;
        }
        case T::Object: case T::Class:
        {
            void* v = OptObject(s, idx);
            if (v && d.type == T::Object && d.ref && !IsA(v, d.ref))
                luaL_error(s, "'%s' takes a %s, not a %s", what, NameStr(NameOf(d.ref)).c_str(), NameStr(NameOf(ClassOf(v))).c_str());
            if (v && d.type == T::Class && d.ref2 && !IsChildOf(v, d.ref2))
                luaL_error(s, "'%s' takes a class based on %s", what, NameStr(NameOf(d.ref2)).c_str());
            Store(s, addr, &v, 4, what);
            return;
        }
        case T::Struct: WriteStruct(s, idx, d.ref, addr, d.vector, d.rotator, what); return;
        case T::Array: luaL_error(s, "'%s' is a dynamic array, which scripts can only read for now", what); return;
        default: luaL_error(s, "'%s' has a type scripts can't write yet", what); return;
        }
    }

    void WriteProp(lua_State* s, int idx, const Prop& d, uint8_t* base, const char* what)
    {
        if (d.dim > 1)
        {
            idx = lua_absindex(s, idx);
            luaL_checktype(s, idx, LUA_TTABLE);
            for (int i = 0; i < d.dim; ++i)
            {
                lua_rawgeti(s, idx, i + 1);
                if (!lua_isnil(s, -1)) WriteElem(s, -1, d, base + d.offset + i * d.size, what);
                lua_pop(s, 1);
            }
        }
        else
            WriteElem(s, idx, d, base + d.offset, what);
    }

    // --- Calling functions -----------------------------------------------------------------------------

    void FreeStrings(const std::vector<const Prop*>& props, uint8_t* base)
    {
        for (const Prop* p : props)
            if (p->type == T::Str)
                for (int i = 0; i < p->dim; ++i) StrAssign(base + p->offset + i * p->size, L"");
    }

    // obj:Function(args...): args start at Lua index 'first'. Returns the return value, then the out parameters.
    int CallFunction(lua_State* s, void* obj, int name, int first)
    {
        void* fn = Script::Function(obj, name);
        if (!fn) return luaL_error(s, "%s has no function '%s'", NameStr(NameOf(ClassOf(obj))).c_str(), NameStr(name).c_str());
        const uint32_t flags = Rd<uint32_t>(fn, kFuncFlags);
        const uint16_t iNative = Rd<uint16_t>(fn, kFuncNative);
        const uint16_t parmsSize = Rd<uint16_t>(fn, kFuncParms);
        std::vector<const Prop*> params, outs, strings;
        const Prop* ret = nullptr;
        Fields(fn, false, [&](void* f) {
            const Prop& d = Describe(f);
            if (d.type == T::None || !(d.flags & CPF_Parm) || d.offset >= parmsSize) return true;
            if (d.flags & CPF_ReturnParm) ret = &d;
            else { params.push_back(&d); if (d.flags & CPF_OutParm) outs.push_back(&d); }
            if (d.type == T::Str) strings.push_back(&d);
            return true;
        });
        const std::string fname = NameStr(name);
        // The parameters found must account for every byte of ParmsSize; otherwise the layout wasn't understood
        // and calling would hand the game garbage.
        size_t covered = 0;
        for (const Prop* d : params) covered = std::max<size_t>(covered, size_t(d->offset) + size_t(d->size) * d->dim);
        if (ret) covered = std::max<size_t>(covered, size_t(ret->offset) + size_t(ret->size) * ret->dim);
        if (parmsSize && (covered == 0 || covered + 3 < parmsSize || covered > parmsSize))
            return luaL_error(s, "%s(): couldn't read its parameters (%d of %d bytes), so it wasn't called", fname.c_str(),
                              int(covered), int(parmsSize));
        const int nargs = std::max(0, lua_gettop(s) - first + 1);
        std::vector<uint8_t> buf(size_t(parmsSize) + 64, 0);   // ProcessEvent's parameters, or the native's locals
        auto given = [&](size_t i) { return int(i) < nargs && !lua_isnil(s, first + int(i)); };
        auto results = [&](const uint8_t* retBase) {
            int n = 0;
            if (ret) { Prop r = *ret; if (retBase != buf.data()) r.offset = 0; PushProp(s, r, retBase); ++n; }
            for (const Prop* o : outs) { PushProp(s, *o, buf.data()); ++n; }
            return n;
        };

        if (!(flags & kFuncNativeFlag) && !iNative)
        {
            for (size_t i = 0; i < params.size(); ++i)
                if (given(i)) WriteProp(s, first + int(i), *params[i], buf.data(), NameStr(params[i]->name).c_str());
            const bool ok = CallEvent(obj, fn, buf.data());
            const int n = ok ? results(buf.data()) : 0;
            FreeStrings(strings, buf.data());
            if (!ok) return luaL_error(s, "%s() crashed inside the game", fname.c_str());
            return n;
        }

        Script::Args args;
        auto local = [&](const Prop& d) { const uint8_t tok = 0x00; void* p = d.p; args.Raw(&tok, 1); args.Raw(&p, 4); };   // EX_LocalVariable
        for (size_t i = 0; i < params.size(); ++i)
        {
            const int a = first + int(i);
            const Prop& d = *params[i];
            if (d.flags & CPF_OutParm)
            {
                if (given(i)) WriteProp(s, a, d, buf.data(), NameStr(d.name).c_str());
                local(d);
                continue;
            }
            if (!given(i)) { args.Skip(); continue; }
            if (d.dim > 1) { WriteProp(s, a, d, buf.data(), NameStr(d.name).c_str()); local(d); continue; }
            switch (d.type)
            {
            case T::Int: args.Int(int(luaL_checkinteger(s, a))); break;
            case T::Byte: args.Byte(uint8_t(luaL_checkinteger(s, a))); break;
            case T::Float: args.Float(float(luaL_checknumber(s, a))); break;
            case T::Bool: args.Bool(lua_toboolean(s, a)); break;
            case T::Name: { size_t n = 0; const char* str = luaL_checklstring(s, a, &n); args.Name(Script::NameAdd(Wide(str, n).c_str())); break; }
            case T::Object: case T::Class:
            {
                void* v = CheckObject(s, a);
                if (d.type == T::Object && d.ref && !IsA(v, d.ref))
                    return luaL_error(s, "%s(): argument %d takes a %s", fname.c_str(), int(i) + 1, NameStr(NameOf(d.ref)).c_str());
                const uint8_t tok = 0x20;   // EX_ObjectConst
                args.Raw(&tok, 1);
                args.Raw(&v, 4);
                break;
            }
            case T::Str: case T::Struct:
                WriteProp(s, a, d, buf.data(), NameStr(d.name).c_str());
                local(d);
                break;
            default:
                return luaL_error(s, "%s(): argument %d has a type scripts can't pass yet", fname.c_str(), int(i) + 1);
            }
        }
        void* code = iNative ? Script::Native(iNative) : Rd<void*>(fn, kFuncCode);
        if (!code) return luaL_error(s, "%s() can't be called from scripts", fname.c_str());
        alignas(16) uint8_t result[1024] = {};
        const bool ok = Script::CallNativeEx(obj, code, args.End(), result, buf.data());
        const int n = ok ? results(result) : 0;
        FreeStrings(strings, buf.data());
        if (ret && ret->type == T::Str) StrAssign(result, L"");
        if (!ok) return luaL_error(s, "%s() crashed inside the game", fname.c_str());
        return n;
    }

    // --- Object metatable ------------------------------------------------------------------------------

    int MethodCall(lua_State* s)
    {
        const int name = int(lua_tointeger(s, lua_upvalueindex(1)));
        if (!luaL_testudata(s, 1, kObjMeta))
            return luaL_error(s, "call %s with ':' (obj:%s(...)), not '.'", NameStr(name).c_str(), NameStr(name).c_str());
        void* obj = CheckObject(s, 1);
        return CallFunction(s, obj, name, 2);
    }

    int ObjIndex(lua_State* s)
    {
        void* obj = CheckObject(s, 1);
        const char* key = luaL_checkstring(s, 2);
        const int name = NameIdx(key);
        if (!name) { lua_pushnil(s); return 1; }
        if (void* prop = FindProp(ClassOf(obj), name))
        {
            PushProp(s, Describe(prop), static_cast<uint8_t*>(obj));
            return 1;
        }
        if (Script::Function(obj, name))
        {
            lua_pushinteger(s, name);
            lua_pushcclosure(s, &MethodCall, 1);
            return 1;
        }
        lua_pushnil(s);
        return 1;
    }

    int ObjNewIndex(lua_State* s)
    {
        void* obj = CheckObject(s, 1);
        const char* key = luaL_checkstring(s, 2);
        const int name = NameIdx(key);
        void* prop = name ? FindProp(ClassOf(obj), name) : nullptr;
        if (!prop) return luaL_error(s, "%s has no property '%s'", NameStr(NameOf(ClassOf(obj))).c_str(), key);
        WriteProp(s, 3, Describe(prop), static_cast<uint8_t*>(obj), key);
        return 0;
    }

    int ObjToString(lua_State* s)
    {
        auto* u = static_cast<LObj*>(luaL_checkudata(s, 1, kObjMeta));
        if (!StillSame(u)) { lua_pushstring(s, "<destroyed object>"); return 1; }
        lua_pushstring(s, FullName(u->p).c_str());
        return 1;
    }

    int ObjEq(lua_State* s)
    {
        auto* a = static_cast<LObj*>(luaL_testudata(s, 1, kObjMeta));
        auto* b = static_cast<LObj*>(luaL_testudata(s, 2, kObjMeta));
        lua_pushboolean(s, a && b && a->p == b->p && a->index == b->index);
        return 1;
    }

    // --- Game API --------------------------------------------------------------------------------------

    void* Pawn() { return g_pawn && Alive(g_pawn) ? g_pawn : nullptr; }
    void* LevelInfo() { void* p = Pawn(); return p ? Rd<void*>(p, kActorLevel) : nullptr; }

    std::unordered_map<std::string, void*> g_classCache;
    void* FindClass(const char* name)
    {
        std::string key = name;
        for (auto& c : key) c = char(tolower(static_cast<unsigned char>(c)));
        auto it = g_classCache.find(key);
        if (it != g_classCache.end() && it->second && Alive(it->second)) return it->second;
        const int n = NameIdx(name), classN = NameIdx("Class");
        if (!n || !classN || !g_objData) return nullptr;
        void** data = Rd<void**>(g_objData, 0);
        const int num = Rd<int>(g_objNum, 0);
        for (int i = 0; data && i < num; ++i)
        {
            void* o = Rd<void*>(data, uintptr_t(i) * 4);
            if (o && NameOf(o) == n && NameOf(ClassOf(o)) == classN)
            {
                g_classCache[key] = o;
                return o;
            }
        }
        return nullptr;
    }

    int L_player(lua_State* s) { PushObject(s, Pawn()); return 1; }
    int L_level(lua_State* s) { PushObject(s, LevelInfo()); return 1; }
    int L_map(lua_State* s) { const std::string m = Utf8(g_map.c_str(), g_map.size()); lua_pushstring(s, m.c_str()); return 1; }
    int L_time(lua_State* s) { void* l = LevelInfo(); lua_pushnumber(s, l ? Rd<float>(l, kLevelTime) : 0.0f); return 1; }
    int L_class(lua_State* s) { PushObject(s, FindClass(luaL_checkstring(s, 1))); return 1; }

    int L_actors(lua_State* s)
    {
        void* cls = lua_isnoneornil(s, 1) ? nullptr : CheckClass(s, 1);
        lua_newtable(s);
        void* pawn = Pawn();
        void* xlevel = pawn ? Rd<void*>(pawn, kActorXLevel) : nullptr;
        void** actors = xlevel ? Rd<void**>(xlevel, kXLevelActors) : nullptr;
        const int count = xlevel ? std::clamp(Rd<int>(xlevel, kXLevelCount), 0, 200000) : 0;
        int n = 0;
        for (int i = 0; actors && i < count; ++i)
        {
            void* a = Rd<void*>(actors, uintptr_t(i) * 4);
            if (!a || (Rd<uint32_t>(a, kActorFlags) & kDeleteMe) || (cls && !IsA(a, cls))) continue;
            PushObject(s, a);
            if (lua_isnil(s, -1)) { lua_pop(s, 1); continue; }
            lua_rawseti(s, -2, ++n);
        }
        return 1;
    }

    int L_objects(lua_State* s)
    {
        void* cls = CheckClass(s, 1);
        lua_newtable(s);
        void** data = g_objData ? Rd<void**>(g_objData, 0) : nullptr;
        const int num = g_objNum ? Rd<int>(g_objNum, 0) : 0;
        int n = 0;
        for (int i = 0; data && i < num && n < 50000; ++i)
        {
            void* o = Rd<void*>(data, uintptr_t(i) * 4);
            if (!o || !IsA(o, cls)) continue;
            PushObject(s, o);
            lua_rawseti(s, -2, ++n);
        }
        return 1;
    }

    const char* TypeName(T t);

    // importtext(obj, property, text [, index]): sets a property from Unreal text syntax with the engine's own parser
    // (UProperty::ImportText, vtable +8Ch, as execSetPropertyText 0x10CF8300 calls it).
    using ImportText_t = const wchar_t*(__fastcall*)(void* prop, void* edx, const wchar_t* text, uint8_t* data, int flags);
    const wchar_t* ImportRaw(void* prop, const wchar_t* text, uint8_t* data)
    {
        __try { return reinterpret_cast<ImportText_t>((*reinterpret_cast<void***>(prop))[0x8C / 4])(prop, nullptr, text, data, 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return reinterpret_cast<const wchar_t*>(-1); }
    }
    int L_importtext(lua_State* s)
    {
        void* obj = CheckObject(s, 1);
        const char* key = luaL_checkstring(s, 2);
        size_t n = 0;
        const char* text = luaL_checklstring(s, 3, &n);
        const int index = int(luaL_optinteger(s, 4, 0));
        const int name = NameIdx(key);
        void* prop = name ? FindProp(ClassOf(obj), name) : nullptr;
        if (!prop) return luaL_error(s, "%s has no property '%s'", NameStr(NameOf(ClassOf(obj))).c_str(), key);
        const Prop& d = Describe(prop);
        if (index < 0 || index >= d.dim) return luaL_error(s, "'%s' has no element %d", key, index);
        const std::wstring w = Wide(text, n);
        const wchar_t* r = ImportRaw(d.p, w.c_str(), static_cast<uint8_t*>(obj) + d.offset + size_t(index) * d.size);
        if (r == reinterpret_cast<const wchar_t*>(-1)) return luaL_error(s, "setting '%s' crashed inside the game", key);
        if (!r) return luaL_error(s, "'%s' couldn't be read as a %s value", text, TypeName(d.type));
        return 0;
    }

    // actor(name): the level's actor with that object name (e.g. "EPeoplesVoiceSoldier3"), or nil.
    void* FindActor(int name)
    {
        void* pawn = Pawn();
        void* xlevel = pawn ? Rd<void*>(pawn, kActorXLevel) : nullptr;
        void** actors = xlevel ? Rd<void**>(xlevel, kXLevelActors) : nullptr;
        const int count = xlevel ? std::clamp(Rd<int>(xlevel, kXLevelCount), 0, 200000) : 0;
        for (int i = 0; actors && name && i < count; ++i)
        {
            void* a = Rd<void*>(actors, uintptr_t(i) * 4);
            if (a && NameOf(a) == name && !(Rd<uint32_t>(a, kActorFlags) & kDeleteMe)) return a;
        }
        return nullptr;
    }
    int L_actor(lua_State* s) { PushObject(s, FindActor(NameIdx(luaL_checkstring(s, 1)))); return 1; }

    // copyprops(dst, src): copies the properties a level designer sets (everything except position, links to runtime
    // actors and runtime state) from src to dst with the engine's own deep copy (UProperty::CopyCompleteValue, vtable
    // +9Ch, as execLocalVariable 0x10CEE960 uses it). An object property is only copied when it points at a non-actor
    // (mesh, sound, class, ...) or at an actor loaded with the map (AI zones, patrol points), so a copied guard never
    // shares the original's weapon, controller or helpers. Returns the number of properties copied.
    using CopyComplete_t = void(__fastcall*)(void* prop, void* edx, void* dest, void* src, void* extra);
    bool CopyRaw(void* prop, void* dest, void* src)
    {
        __try { reinterpret_cast<CopyComplete_t>((*reinterpret_cast<void***>(prop))[0x9C / 4])(prop, nullptr, dest, src, nullptr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadBool(void* obj, const char* name)
    {
        const int n = NameIdx(name);
        void* prop = n ? FindProp(ClassOf(obj), n) : nullptr;
        if (!prop) return false;
        const Prop& d = Describe(prop);
        return d.type == T::Bool && (Rd<uint32_t>(obj, d.offset) & d.mask) != 0;
    }
    int L_copyprops(lua_State* s)
    {
        void* dst = CheckObject(s, 1);
        void* src = CheckObject(s, 2);
        if (dst == src) return 0;
        if (!IsA(dst, ClassOf(src)) && !IsA(src, ClassOf(dst)))
            return luaL_error(s, "copyprops needs objects of the same class (%s, %s)", NameStr(NameOf(ClassOf(dst))).c_str(),
                              NameStr(NameOf(ClassOf(src))).c_str());
        static const std::unordered_set<std::string> kSkip = {
            "location", "rotation", "relativelocation", "relativerotation", "collocation", "colrotation", "m_prevpos",
            "m_prevrot", "floor", "velocity", "acceleration", "oldlocation", "tag", "event", "name", "level", "xlevel",
            "region", "physicsvolume", "base", "owner", "instigator", "controller", "inventory", "weapon", "bhidden",
            "physics", "m_pdummy", "emotions", "touching", "attached", "deleted", "bstatic", "bnodelete", "bdeleteme",
            "bpendingdelete", "lastrendertime", "m_lastsplashtime", "bvel_timer", "bvel_ind", "latentfloat",
            "timerrate", "timercounter", "binitialized", "bticked", "bstasis", "zone", "rendertime",
            // native handles each actor gets for itself (its Havok body and phantom, lip sync, occlusion, impacts):
            // a copy holding the original's would share them (copied characters didn't fall when killed)
            "havokrigidbody", "havokphantom", "m_hlipsynchdata", "dynoccid", "impactid", "ninraincount" };
        void* actorClass = FindClass("Actor");
        // An actor that came from the map file has a linker (UObject +10h); one spawned while playing (the original's
        // weapon, controller, helpers) doesn't. Links to map actors (AI zones, patrol points, triggers) are kept.
        auto fixedOrNotActor = [&](void* v) {
            if (!v || !Alive(v) || !actorClass || !IsA(v, actorClass)) return true;
            return Rd<void*>(v, 0x10) != nullptr || ReadBool(v, "bStatic") || ReadBool(v, "bNoDelete");
        };
        // Copy from the shared base class's fields only.
        void* cls = IsA(dst, ClassOf(src)) ? ClassOf(src) : ClassOf(dst);
        int copied = 0;
        Fields(cls, true, [&](void* f) {
            const Prop& d = Describe(f);
            if (d.type == T::None || d.type == T::Other) return true;
            // Never the engine's own bookkeeping: UObject's fields (ObjectInternal = index/hash/state, Outer, ObjectFlags,
            // Name, Class all sit below +28h) and properties flagged native (400h), transient-like (800h) or 400000h
            // (from Core.u/Engine.u: ObjectInternal 402h, XLevel 802h, Level/Region 400002h, Touching 420002h).
            if (d.offset < 0x28 || (d.flags & 0x404C00)) return true;
            std::string nm = NameStr(d.name);
            for (auto& c : nm) c = char(tolower(static_cast<unsigned char>(c)));
            if (kSkip.count(nm)) return true;
            if (d.type == T::Struct && (d.vector || d.rotator) && nm != "drawscale3d" && nm != "prepivot") return true;
            uint8_t* sa = static_cast<uint8_t*>(src) + d.offset;
            uint8_t* da = static_cast<uint8_t*>(dst) + d.offset;
            if (d.type == T::Object)
                for (int i = 0; i < d.dim; ++i)
                    if (!fixedOrNotActor(Rd<void*>(sa, uintptr_t(i) * 4))) return true;
            if (d.type == T::Array && Describe(d.ref).type == T::Object)
            {
                void** data = Rd<void**>(sa, 0);
                const int num = std::clamp(Rd<int>(sa, 4), 0, 4096);
                for (int i = 0; data && i < num; ++i)
                    if (!fixedOrNotActor(Rd<void*>(data, uintptr_t(i) * 4))) return true;
            }
            if (CopyRaw(d.p, da, sa)) ++copied;
            return true;
        });
        lua_pushinteger(s, copied);
        return 1;
    }

    // copyprop(dst, src, name): deep-copies one property (same class) with the engine's copy, whatever it is.
    int L_copyprop(lua_State* s)
    {
        void* dst = CheckObject(s, 1);
        void* src = CheckObject(s, 2);
        const char* key = luaL_checkstring(s, 3);
        if (!IsA(dst, ClassOf(src)) && !IsA(src, ClassOf(dst))) return luaL_error(s, "copyprop needs objects of the same class");
        const int name = NameIdx(key);
        void* prop = name ? FindProp(ClassOf(src), name) : nullptr;
        if (!prop) return luaL_error(s, "%s has no property '%s'", NameStr(NameOf(ClassOf(src))).c_str(), key);
        const Prop& d = Describe(prop);
        if (d.offset < 0x28) return luaL_error(s, "'%s' is the engine's own bookkeeping", key);
        if (!CopyRaw(d.p, static_cast<uint8_t*>(dst) + d.offset, static_cast<uint8_t*>(src) + d.offset))
            return luaL_error(s, "copying '%s' crashed inside the game", key);
        return 0;
    }

    int L_modcount(lua_State* s) { lua_pushinteger(s, lua_Integer(Mods::Loaded().size())); return 1; }
    int L_modname(lua_State* s)
    {
        const auto& mods = Mods::Loaded();
        const int i = int(luaL_checkinteger(s, 1));
        if (i < 0 || i >= int(mods.size())) return 0;
        const std::string n = Utf8(mods[size_t(i)].name.c_str(), mods[size_t(i)].name.size());
        lua_pushstring(s, n.c_str());
        return 1;
    }
    // core.levelfile(modIndex, map): the text of Mods\<mod>\Levels\<map>.txt, or nil.
    int L_levelfile(lua_State* s)
    {
        const auto& mods = Mods::Loaded();
        const int i = int(luaL_checkinteger(s, 1));
        const char* map = luaL_checkstring(s, 2);
        if (i < 0 || i >= int(mods.size()) || !g_settings.levelChanges || strpbrk(map, "\\/:")) return 0;
        FILE* f = _wfopen((mods[size_t(i)].dir + L"Levels\\" + Wide(map, strlen(map)) + L".txt").c_str(), L"rb");
        if (!f) return 0;
        std::string text;
        char buf[8192];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
        fclose(f);
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);
        lua_pushlstring(s, text.data(), text.size());
        return 1;
    }
    int L_leveldue(lua_State* s)
    {
        if (g_levelDue) Editor::LevelFilesApplied();
        lua_pushboolean(s, g_levelDue);
        g_levelDue = false;
        return 1;
    }
    int L_modfolder(lua_State* s)
    {
        const auto& mods = Mods::Loaded();
        const int i = int(luaL_checkinteger(s, 1));
        if (i < 0 || i >= int(mods.size())) return 0;
        const std::string n = Utf8(mods[size_t(i)].folder.c_str(), mods[size_t(i)].folder.size());
        lua_pushstring(s, n.c_str());
        return 1;
    }
    // core.record(folder, blocks): what a mod's level file did at this mission's start, for the level editor's Save.
    // Each block: { kind, head, label, copy, class, name, obj, props = { {key, value}, ... } }.
    int L_record(lua_State* s)
    {
        const char* folder = luaL_checkstring(s, 1);
        luaL_checktype(s, 2, LUA_TTABLE);
        std::vector<Editor::LevelBlock> blocks;
        const lua_Integer n = luaL_len(s, 2);
        for (lua_Integer i = 1; i <= n; ++i)
        {
            lua_rawgeti(s, 2, i);
            if (lua_istable(s, -1))
            {
                Editor::LevelBlock b;
                auto str = [&](const char* k, std::string& out) {
                    lua_getfield(s, -1, k);
                    if (lua_type(s, -1) == LUA_TSTRING) out = lua_tostring(s, -1);
                    lua_pop(s, 1);
                };
                str("kind", b.kind); str("head", b.head); str("label", b.label); str("copy", b.copy); str("class", b.cls); str("name", b.name);
                lua_getfield(s, -1, "obj");
                if (auto* u = static_cast<LObj*>(luaL_testudata(s, -1, kObjMeta)); u && StillSame(u)) b.obj = u->p;
                lua_pop(s, 1);
                lua_getfield(s, -1, "props");
                if (lua_istable(s, -1))
                {
                    const lua_Integer m = luaL_len(s, -1);
                    for (lua_Integer j = 1; j <= m; ++j)
                    {
                        lua_rawgeti(s, -1, j);
                        if (lua_istable(s, -1))
                        {
                            lua_rawgeti(s, -1, 1);
                            lua_rawgeti(s, -2, 2);
                            if (lua_type(s, -2) == LUA_TSTRING && lua_type(s, -1) == LUA_TSTRING)
                                b.props.emplace_back(lua_tostring(s, -2), lua_tostring(s, -1));
                            lua_pop(s, 2);
                        }
                        lua_pop(s, 1);
                    }
                }
                lua_pop(s, 1);
                blocks.push_back(std::move(b));
            }
            lua_pop(s, 1);
        }
        Editor::Record(folder, std::move(blocks));
        return 0;
    }
    // core.diffprops(a, b): "Name: a's value | b's value" for each property (engine internals too) whose text differs.
    int L_diffprops(lua_State* s)
    {
        void* a = CheckObject(s, 1);
        void* b = CheckObject(s, 2);
        lua_newtable(s);
        if (ClassOf(a) != ClassOf(b)) return 1;
        std::vector<Engine::PropRow> rows;
        Engine::Properties(a, rows);
        int n = 0;
        auto cut = [](std::string v) { if (v.size() > 90) { v.resize(87); v += "..."; } return v; };
        for (const auto& r : rows)
            for (int i = 0; i < r.dim && n < 300; ++i)
            {
                std::string va, vb;
                if (!Engine::Export(a, r.prop, i, va) || !Engine::Export(b, r.prop, i, vb) || va == vb) continue;
                std::string line = r.dim > 1 ? r.name + "(" + std::to_string(i) + ")" : r.name;
                line += (r.internal ? " [internal]: " : ": ") + cut(va) + " | " + cut(vb);
                lua_pushstring(s, line.c_str());
                lua_rawseti(s, -2, ++n);
            }
        return 1;
    }

    // The Niko plush (plush.cpp): the prelude gives and tracks it, C++ draws it and meows.
    int L_plush_on(lua_State* s) { lua_pushboolean(s, Plush::Active()); return 1; }
    int L_plush_draw(lua_State* s)
    {
        auto* samObj = static_cast<LObj*>(luaL_testudata(s, 1, kObjMeta));
        void* sam = samObj && StillSame(samObj) ? samObj->p : nullptr;
        std::vector<void*> list;
        if (lua_istable(s, 2))
        {
            const lua_Integer n = luaL_len(s, 2);
            for (lua_Integer i = 1; i <= n && i <= 64; ++i)
            {
                lua_rawgeti(s, 2, i);
                if (auto* u = static_cast<LObj*>(luaL_testudata(s, -1, kObjMeta)); u && StillSame(u)) list.push_back(u->p);
                lua_pop(s, 1);
            }
        }
        // 3: the plush in his hand, 4: { x, y, z, yaw } where to draw it.
        auto* handObj = static_cast<LObj*>(luaL_testudata(s, 3, kObjMeta));
        void* handActor = handObj && StillSame(handObj) ? handObj->p : nullptr;
        float hand[3] = {};
        int yaw = 0;
        bool haveHand = false;
        if (handActor && lua_istable(s, 4))
        {
            for (int k = 0; k < 4; ++k)
            {
                lua_rawgeti(s, 4, k + 1);
                const double v = lua_tonumber(s, -1);
                lua_pop(s, 1);
                if (k < 3) hand[k] = float(v); else yaw = int(v);
            }
            haveHand = true;
        }
        Plush::SetItems(sam, list.data(), int(list.size()), handActor, haveHand ? hand : nullptr, yaw, int(luaL_optinteger(s, 5, 5)));
        return 0;
    }
    int L_plush_meow(lua_State* s)
    {
        const float p[3] = { float(luaL_checknumber(s, 1)), float(luaL_checknumber(s, 2)), float(luaL_checknumber(s, 3)) };
        Plush::Meow(p, int(luaL_optinteger(s, 4, 0)));
        return 0;
    }
    int L_plush_noise(lua_State* s) { lua_pushnumber(s, g_settings.nikoPlushNoise); return 1; }
    int L_plush_camdist(lua_State* s)   // distance from the camera to a point (-1 if there's no camera yet)
    {
        float cam[3], fwd[3];
        if (!Editor::RenderCamera(cam, fwd)) { lua_pushnumber(s, -1); return 1; }
        const float dx = float(luaL_checknumber(s, 1)) - cam[0], dy = float(luaL_checknumber(s, 2)) - cam[1], dz = float(luaL_checknumber(s, 3)) - cam[2];
        lua_pushnumber(s, std::sqrt(dx * dx + dy * dy + dz * dz));
        return 1;
    }

    // EChemicalFlare.SetDefaults (run by PostBeginPlay on every spawn) loads the flare's mesh from a package Chaos
    // Theory doesn't have whenever StaticMesh is None, which it always is: a failed package search every time a plush
    // is made. Its bytecode is made to return at once: EX_Return (04h) EX_Nothing (0Bh) over its first statement, an
    // EX_JumpIfNot (07h). The bytecode is the TArray in the UFunction (UStruct::Script) found by that first byte.
    int PatchReturn(void* func)   // 1 patched, 2 already, 0 not found
    {
        __try
        {
            for (uintptr_t off = 0x20; off < 0x100; off += 4)
            {
                uint8_t* d = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(func) + off);
                const int num = *reinterpret_cast<int*>(static_cast<uint8_t*>(func) + off + 4);
                const int max = *reinterpret_cast<int*>(static_cast<uint8_t*>(func) + off + 8);
                if (reinterpret_cast<uintptr_t>(d) < 0x10000 || num < 8 || num > 4096 || max < num) continue;
                if (d[0] == 0x04 && d[1] == 0x0B) return 2;
                if (d[0] != 0x07) continue;
                d[0] = 0x04;
                d[1] = 0x0B;
                return 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    int L_plush_quiet(lua_State* s)
    {
        void* cls = CheckObject(s, 1);
        const int fn = NameIdx("SetDefaults"), function = NameIdx("Function");
        void** data = g_objData ? Rd<void**>(g_objData, 0) : nullptr;
        const int num = g_objNum ? Rd<int>(g_objNum, 0) : 0;
        void* func = nullptr;
        for (int i = 0; fn && function && data && i < num && !func; ++i)
        {
            void* o = Rd<void*>(data, uintptr_t(i) * 4);
            if (o && NameOf(o) == fn && Rd<void*>(o, 0x18) == cls && NameOf(ClassOf(o)) == function) func = o;
        }
        const int r = func ? PatchReturn(func) : -1;
        lua_pushstring(s, r == 1 ? "patched" : r == 2 ? "already patched" : r == 0 ? "bytecode not recognised" : "function not found");
        return 1;
    }

    int L_valid(lua_State* s)
    {
        auto* u = static_cast<LObj*>(luaL_testudata(s, 1, kObjMeta));
        lua_pushboolean(s, u && StillSame(u));
        return 1;
    }
    int L_isa(lua_State* s) { void* o = CheckObject(s, 1); lua_pushboolean(s, IsA(o, CheckClass(s, 2))); return 1; }
    int L_nameof(lua_State* s) { lua_pushstring(s, NameStr(NameOf(CheckObject(s, 1))).c_str()); return 1; }
    int L_classof(lua_State* s) { PushObject(s, ClassOf(CheckObject(s, 1))); return 1; }
    int L_fullname(lua_State* s) { lua_pushstring(s, FullName(CheckObject(s, 1)).c_str()); return 1; }

    const char* TypeName(T t)
    {
        switch (t)
        {
        case T::Byte: return "byte"; case T::Int: return "int"; case T::Bool: return "bool"; case T::Float: return "float";
        case T::Name: return "name"; case T::Str: return "string"; case T::Object: return "object"; case T::Class: return "class";
        case T::Struct: return "struct"; case T::Array: return "array"; default: return "?";
        }
    }

    std::string Short(lua_State* s, int idx)
    {
        idx = lua_absindex(s, idx);
        switch (lua_type(s, idx))
        {
        case LUA_TTABLE:
        {
            std::string out = "{";
            int count = 0;
            lua_pushnil(s);
            while (lua_next(s, idx) && count < 8)
            {
                if (count++) out += ", ";
                if (lua_type(s, -2) == LUA_TSTRING) { out += lua_tostring(s, -2); out += "="; }
                const int t = lua_type(s, -1);
                out += t == LUA_TTABLE ? std::string("{...}") : std::string(luaL_tolstring(s, -1, nullptr));
                if (t != LUA_TTABLE) lua_pop(s, 1);
                lua_pop(s, 1);
            }
            if (count >= 8) { lua_pop(s, 2); out += ", ..."; }
            return out + "}";
        }
        default:
        {
            std::string out = luaL_tolstring(s, idx, nullptr);
            lua_pop(s, 1);
            return out;
        }
        }
    }

    // dump(obj [, filter]): logs every property (with its value) and function of the object's class.
    int L_dump(lua_State* s)
    {
        void* obj = CheckObject(s, 1);
        std::string filter = luaL_optstring(s, 2, "");
        for (auto& c : filter) c = char(tolower(static_cast<unsigned char>(c)));
        LOG("[LUA] dump of %s:", FullName(obj).c_str());
        std::string funcs;
        int props = 0, nfuncs = 0;
        std::unordered_set<int> seen;
        Fields(ClassOf(obj), true, [&](void* f) {
            const int n = NameOf(f);
            if (!seen.insert(n).second) return true;
            std::string nm = NameStr(n), low = nm;
            for (auto& c : low) c = char(tolower(static_cast<unsigned char>(c)));
            if (!filter.empty() && low.find(filter) == std::string::npos) return true;
            const Prop& d = Describe(f);
            if (d.type != T::None)
            {
                PushProp(s, d, static_cast<uint8_t*>(obj));
                const std::string v = Short(s, -1);
                lua_pop(s, 1);
                LOG("[LUA]   %s (%s%s) = %s", nm.c_str(), TypeName(d.type), d.dim > 1 ? "[]" : "", v.c_str());
                ++props;
            }
            else if (Is(NameStr(NameOf(ClassOf(f))), "Function"))
            {
                funcs += (nfuncs++ ? ", " : "") + nm;
                if (funcs.size() > 1500) { LOG("[LUA]   functions: %s", funcs.c_str()); funcs.clear(); nfuncs = 0; }
            }
            return true;
        });
        if (!funcs.empty()) LOG("[LUA]   functions: %s", funcs.c_str());
        LOG("[LUA] (%d properties)", props);
        return 0;
    }

    // --- Input -----------------------------------------------------------------------------------------

    int KeyCode(const char* name)
    {
        std::string k = name;
        for (auto& c : k) c = char(toupper(static_cast<unsigned char>(c)));
        if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
        if (k.size() >= 2 && k[0] == 'F' && isdigit(static_cast<unsigned char>(k[1])))
        {
            const int n = atoi(k.c_str() + 1);
            if (n >= 1 && n <= 24) return VK_F1 + n - 1;
        }
        if (k.size() == 4 && k.compare(0, 3, "NUM") == 0 && isdigit(static_cast<unsigned char>(k[3]))) return VK_NUMPAD0 + (k[3] - '0');
        static const std::pair<const char*, int> kNames[] = {
            { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "RETURN", VK_RETURN }, { "ESCAPE", VK_ESCAPE }, { "ESC", VK_ESCAPE },
            { "TAB", VK_TAB }, { "SHIFT", VK_SHIFT }, { "CTRL", VK_CONTROL }, { "CONTROL", VK_CONTROL }, { "ALT", VK_MENU },
            { "BACKSPACE", VK_BACK }, { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
            { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END }, { "PAGEUP", VK_PRIOR },
            { "PAGEDOWN", VK_NEXT }, { "MOUSE1", VK_LBUTTON }, { "MOUSE2", VK_RBUTTON }, { "MOUSE3", VK_MBUTTON },
            { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 }, { "NUMPLUS", VK_ADD }, { "NUMMINUS", VK_SUBTRACT },
            { "NUMSTAR", VK_MULTIPLY }, { "NUMSLASH", VK_DIVIDE }, { "NUMDOT", VK_DECIMAL },
        };
        for (const auto& [n, vk] : kNames)
            if (k == n) return vk;
        return 0;
    }
    int CheckKey(lua_State* s)
    {
        const char* name = luaL_checkstring(s, 1);
        const int vk = KeyCode(name);
        if (!vk) luaL_error(s, "unknown key '%s'", name);
        return vk;
    }
    int L_key(lua_State* s) { const int vk = CheckKey(s); lua_pushboolean(s, (g_keys[vk] & 0x80) != 0); return 1; }
    int L_pressed(lua_State* s) { const int vk = CheckKey(s); lua_pushboolean(s, (g_keys[vk] & 0x80) && !(g_prevKeys[vk] & 0x80)); return 1; }

    void ReadKeys()
    {
        memcpy(g_prevKeys, g_keys, sizeof(g_keys));
        // The game thread's own keyboard state (one call; it owns the window and pumps its messages).
        if (!(Game::hWnd && GetForegroundWindow() == Game::hWnd) || !GetKeyboardState(g_keys))
            memset(g_keys, 0, sizeof(g_keys));
    }

    // --- Internals for the prelude ---------------------------------------------------------------------

    int L_log(lua_State* s)
    {
        const char* mod = luaL_checkstring(s, 1);
        std::string line;
        const int n = lua_gettop(s);
        for (int i = 2; i <= n; ++i)
        {
            if (i > 2) line += "  ";
            line += luaL_tolstring(s, i, nullptr);
            lua_pop(s, 1);
        }
        LOG("[LUA] %s: %s", mod, line.c_str());
        return 0;
    }

    int L_traceback(lua_State* s)
    {
        const char* msg = lua_tostring(s, 1);
        luaL_traceback(s, s, msg ? msg : "(error object is not a string)", 1);
        return 1;
    }

    // core.load(modIndex, relativePath, env): compiles Mods\<mod>\Scripts\<path> (text only) with 'env' as its globals.
    int L_load(lua_State* s)
    {
        const int index = int(luaL_checkinteger(s, 1));
        const char* rel = luaL_checkstring(s, 2);
        luaL_checktype(s, 3, LUA_TTABLE);
        const auto& mods = Mods::Loaded();
        if (index < 0 || index >= int(mods.size())) return luaL_error(s, "bad mod index");
        if (strstr(rel, "..") || strchr(rel, ':')) return luaL_error(s, "scripts can only load files from their own Scripts folder");
        const std::wstring path = mods[size_t(index)].dir + L"Scripts\\" + Wide(rel, strlen(rel));
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { lua_pushnil(s); lua_pushfstring(s, "can't open Scripts\\%s", rel); return 2; }
        std::string src;
        char buf[8192];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, got);
        fclose(f);
        if (src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF) src.erase(0, 3);   // UTF-8 BOM
        const std::string chunk = "@" + Utf8(mods[size_t(index)].folder.c_str(), mods[size_t(index)].folder.size()) + "/Scripts/" + rel;
        if (luaL_loadbufferx(s, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK)
        {
            lua_pushnil(s);
            lua_insert(s, -2);
            return 2;
        }
        lua_pushvalue(s, 3);
        lua_setupvalue(s, -2, 1);   // _ENV
        return 1;
    }

    // core.scripts(modIndex): the .lua files directly in Mods\<mod>\Scripts\, sorted.
    int L_scripts(lua_State* s)
    {
        const int index = int(luaL_checkinteger(s, 1));
        const auto& mods = Mods::Loaded();
        lua_newtable(s);
        if (index < 0 || index >= int(mods.size())) return 1;
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((mods[size_t(index)].dir + L"Scripts\\*.lua").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (size_t i = 0; i < names.size(); ++i)
        {
            const std::string u = Utf8(names[i].c_str(), names[i].size());
            lua_pushstring(s, u.c_str());
            lua_rawseti(s, -2, int(i) + 1);
        }
        return 1;
    }

    int L_arm(lua_State*)   // a fresh one-second limit for the next handler
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_deadline.QuadPart = now.QuadPart + g_freq.QuadPart;
        return 0;
    }

    int L_setvector(lua_State* s) { luaL_checktype(s, 1, LUA_TTABLE); lua_pushvalue(s, 1); lua_setfield(s, LUA_REGISTRYINDEX, "SCCT.vector"); return 0; }

    int StructIndex(lua_State* s)   // __index of struct tables: a field name in any case
    {
        if (lua_type(s, 2) != LUA_TSTRING) { lua_pushnil(s); return 1; }
        GetFieldCI(s, 1, lua_tostring(s, 2));
        return 1;
    }

    void TimeHook(lua_State* s, lua_Debug*)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_deadline.QuadPart && now.QuadPart > g_deadline.QuadPart)
        {
            g_deadline.QuadPart = 0;   // one error per call
            luaL_error(s, "the script ran for over a second and was stopped (an endless loop?)");
        }
    }

    // The Lua half: mod environments, events, timers, vectors.
    const char* const kPrelude = R"LUA(
local core, API = ...
local mods, handlers, timers = {}, {}, {}
local traceback = core.traceback

-- ---- the Niko plush (CLUSTERFUCK's unlock; drawing and meows in plush.cpp) ----
-- An EChemicalFlare is an inventory item the player controller throws like a grenade (bIsProjectile). Sam is kept
-- at KEEP of them; one that leaves his hand (EGameplayObject.Throw: s_Flying, detached) meows and is tracked: every
-- hit (a sharp turn or drop in its velocity) meows softer and makes an AI_HEAR_OBJECT (78) noise guards investigate.
-- At most WORLD lie around (the oldest goes). On a new map (or a loaded save) the ones Sam already carries are his.
local PLUSH = { class = "EChemicalFlare", path = "EchelonIngredient.EChemicalFlare", keep = 3, world = 3 }
local plush = { lvl = false, cls = false, mine = {}, thrown = {}, blocked = 0, drawing = false, failed = false }

-- The flare's own light: its Use (which some throws call) lights an EDynamicLight unless it's already Used, and its
-- Tick only burns down while GlowTime > 0. A plush has neither, nor any heat.
local function plush_unflare(o)
  pcall(function()
    o.Used = true; o.GlowTime = 0; o.HeatIntensity = 0
    o.DrawType = 0   -- DT_None: not the flare's own model (the yellow stick); plush.cpp draws Niko
    local l = o.Light
    if l then l.LightType = 0; l:Destroy(); o.Light = nil end
  end)
end

local function plush_state(o)
  local ok, st = pcall(function() return o:GetStateName() end)
  return ok and tostring(st) or ""
end

-- The class (by its bare name; loaded from its package if nothing has loaded it yet).
local function plush_class(sam)
  local c = API.class(PLUSH.class)
  if c then return c end
  local ok, r = pcall(function() return sam:DynamicLoadObject(PLUSH.path, API.class("Class")) end)
  if ok and r then return r end
  return false
end

local function plush_tick()
  if not core.plush_on() then
    if plush.drawing then core.plush_draw(nil, {}); plush.drawing = false end
    return
  end
  local sam = API.player()
  if not sam then return end
  local lvl = API.level()
  if lvl ~= plush.lvl then
    plush.lvl, plush.mine, plush.thrown, plush.blocked, plush.gave = lvl, {}, {}, 0, false
    plush.cls = plush_class(sam)
    if not plush.cls then
      if not plush.noclass then core.log("Niko plush", "ERROR: can't find or load " .. PLUSH.path); plush.noclass = true end
      return
    end
    core.log("Niko plush", "ready: Sam is given " .. PLUSH.keep .. " (" .. PLUSH.path .. ")")
    if not plush.quiet then
      plush.quiet = true
      local ok, r = pcall(core.plush_quiet, plush.cls)
      core.log("Niko plush", "skipping the flare's missing-mesh load on spawn: " .. tostring(r))
    end
    plush.outs = 0
    local ctrl = sam.Controller
    for _, a in ipairs(API.actors(plush.cls)) do
      if a.Owner == ctrl or a.Owner == sam then
        plush_unflare(a)
        if plush_state(a) == "s_Flying" or (not a.bHidden and a.Base == nil) then
          plush.thrown[#plush.thrown + 1] = { o = a, v = a.Velocity, hit = -1, at = -1 }
        else
          plush.mine[#plush.mine + 1] = a
        end
      end
    end
  end
  if not plush.cls then return end
  local ctrl, inv = sam.Controller, sam.FullInventory
  if not ctrl or not inv then return end
  local now = API.time()

  -- Carried ones, and the one that just left his hand.
  local carried = {}
  for _, o in ipairs(plush.mine) do
    if API.valid(o) then
      if o.Light or o.DrawType ~= 0 then plush_unflare(o) end
      if plush_state(o) == "s_Flying" and sam.HandItem ~= o then
        local l = o.Location
        core.plush_meow(l.X, l.Y, l.Z, 0)
        plush.thrown[#plush.thrown + 1] = { o = o, v = o.Velocity, hit = -1, at = now }
      else
        carried[#carried + 1] = o
      end
    end
  end
  plush.mine = carried

  -- Top up the inventory, one a tick.
  if #carried < PLUSH.keep and now >= plush.blocked then
    local t0 = os.clock()
    local ok, it = pcall(function() return sam:Spawn(plush.cls, ctrl) end)
    local ms = (os.clock() - t0) * 1000
    if ms > 15 then core.log("Niko plush", ("spawning a plush took %.0f ms"):format(ms)) end
    if ok and it then
      pcall(function() it:SetCollisionSize(9, 13) end)   -- the plush's size: it rests on its bottom
      plush_unflare(it)
      local can, why = pcall(function() return inv:CanAddItem(it) end)
      local added, err = false, nil
      if can and why then added, err = pcall(function() it:Add(ctrl, ctrl, inv) end)
      else err = can and "the inventory refused it (CanAddItem false)" or tostring(why) end
      if added then
        plush.mine[#plush.mine + 1] = it
        if not plush.gave then plush.gave = true; core.log("Niko plush", "a plush went into Sam's inventory") end
      else
        pcall(function() it:Destroy() end); plush.blocked = now + 10
        core.log("Niko plush", "couldn't add a plush to the inventory: " .. tostring(err))
      end
    else
      plush.blocked = now + 10
      core.log("Niko plush", "couldn't spawn a plush: " .. tostring(it))
    end
  end

  -- Thrown ones: hits, rest, and no more than WORLD of them.
  local alive = {}
  for _, t in ipairs(plush.thrown) do
    if API.valid(t.o) then
      if t.o.Light or t.o.DrawType ~= 0 then plush_unflare(t.o) end
      local v = t.o.Velocity
      local pv, cv = t.v:len(), v:len()
      if pv > 120 and now - t.hit > 0.25 and (cv < pv * 0.6 or t.v:dot(v) < pv * cv * 0.6) then
        t.hit = now
        local l = t.o.Location
        if now - t.at > 0.2 then core.plush_meow(l.X, l.Y, l.Z, 1) end   -- (not over the throw's own meow)
        pcall(function() t.o:MakeNoise(core.plush_noise(), 78, l, t.o) end)
      end
      t.v = v
      alive[#alive + 1] = t
    end
  end
  while #alive > PLUSH.world do
    local t = table.remove(alive, 1)
    pcall(function() t.o:Destroy() end)
  end
  plush.thrown = alive

  -- What to draw: the one in his hand, and the thrown ones.
  -- The equipped plush is his HandItem, but it's only out in his hand (attached, and shown) while the game shows it;
  -- equipped or holstered it's hidden and stays where it was.
  local draw = {}
  local inhand = false
  local held = sam.HandItem
  if held and API.valid(held) and held.Class == plush.cls then   -- (what the game does with it, for the log)
    local sl = sam.Location
    local key = ("hidden %s, on Sam %s, state %s, camera %.0f units from Sam"):format(tostring(held.bHidden), tostring(held.Base == sam),
      plush_state(held), core.plush_camdist(sl.X, sl.Y, sl.Z))
    if key:gsub(", camera.*", "") ~= (plush.heldKey or ""):gsub(", camera.*", "") and (plush.heldLogs or 0) < 16 then
      plush.heldKey, plush.heldLogs = key, (plush.heldLogs or 0) + 1
      core.log("Niko plush", "held: " .. key)
    end
  end
  -- Out = shown. Hidden again with the camera close to Sam is the game hiding his gear from a close camera, not a
  -- holster: it stays drawn then.
  if sam.HandItem and API.valid(sam.HandItem) then
    for _, o in ipairs(plush.mine) do
      if o == sam.HandItem then
        local out = not o.bHidden
        if out then plush.out = o
        elseif plush.out == o then
          local sl = sam.Location
          local d = core.plush_camdist(sl.X, sl.Y, sl.Z)
          if d >= 0 and d < 140 then out = true else plush.out = nil end
        end
        if out then draw[#draw + 1] = o; inhand = true end
      end
    end
  end
  if inhand and not plush.inhand and (plush.outs or 0) < 8 then
    plush.outs = (plush.outs or 0) + 1
    core.log("Niko plush", ("taken out at %.2f s"):format(now))
  end
  plush.inhand = inhand
  -- A held item isn't attached to the hand until it's thrown: draw that one at Sam's weapon bone.
  local hand = nil
  if false then pcall(function()   -- (the bone lookup isn't needed: a shown plush is attached to his hand)
    local sl = sam.Location
    local tried = {}
    -- (A bone the skeleton doesn't have gives the origin or Sam's own position: neither counts.)
    for _, b in ipairs({ plush.bone or false, sam.WeaponBone, "B R Hand", "WeaponBone" }) do
      if b and b ~= "None" and not tried[b] then
        tried[b] = true
        local ok, c = pcall(function() return sam:GetBoneCoords(b, false) end)
        local d = ok and c and c.Origin and (c.Origin - sl):len() or -1
        if d > 5 and d < 200 then
          hand = { c.Origin.X, c.Origin.Y, c.Origin.Z, sam.Rotation.Yaw }
          if plush.bone ~= b then
            plush.bone = b
            core.log("Niko plush", ("held at bone '%s', %.0f units from Sam"):format(b, (c.Origin - sl):len()))
          end
          break
        end
      end
    end
    if not hand and not plush.nobone then
      plush.nobone = true
      core.log("Niko plush", "no hand bone near Sam (WeaponBone '" .. tostring(sam.WeaponBone) .. "', B R Hand): drawn where the item is")
    end
  end) end
  local vision = 5
  pcall(function() local g = ctrl.Goggle; if g then vision = g.CurrentMode end end)
  for _, t in ipairs(alive) do draw[#draw + 1] = t.o end
  core.plush_draw(sam, draw, inhand and sam.HandItem or nil, hand, vision)
  plush.drawing = #draw > 0
end

local function report(mod, err) core.log(mod, "ERROR: " .. tostring(err)) end

local arm = core.arm
local function call(mod, fn, ...)
  arm()
  local ok, err = xpcall(fn, traceback, ...)
  if not ok then report(mod, err) end
  return ok
end

-- Vectors: vec(x, y, z), with + - * / and :len(), :dist(), :dot(), :normal(). Vector properties come back as these.
local V = {}
V.__index = V
local function vec(x, y, z) return setmetatable({ X = x or 0, Y = y or 0, Z = z or 0 }, V) end
V.__add = function(a, b) return vec(a.X + b.X, a.Y + b.Y, a.Z + b.Z) end
V.__sub = function(a, b) return vec(a.X - b.X, a.Y - b.Y, a.Z - b.Z) end
V.__mul = function(a, b)
  if type(a) == "number" then return vec(a * b.X, a * b.Y, a * b.Z) end
  if type(b) == "number" then return vec(a.X * b, a.Y * b, a.Z * b) end
  return vec(a.X * b.X, a.Y * b.Y, a.Z * b.Z)
end
V.__div = function(a, b) return vec(a.X / b, a.Y / b, a.Z / b) end
V.__unm = function(a) return vec(-a.X, -a.Y, -a.Z) end
V.__eq = function(a, b) return a.X == b.X and a.Y == b.Y and a.Z == b.Z end
V.__tostring = function(a) return string.format("(%.1f, %.1f, %.1f)", a.X, a.Y, a.Z) end
function V.len(a) return math.sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z) end
function V.dist(a, b) return (a - b):len() end
function V.dot(a, b) return a.X * b.X + a.Y * b.Y + a.Z * b.Z end
function V.normal(a) local l = a:len(); if l == 0 then return vec() end return a / l end
core.setvector(V)
API.vec = vec

-- Timers (game time: they don't run while the game is paused or in menus).
local now = 0
local function add(mod, delay, fn, rep, co)
  local t = { mod = mod, at = now + (tonumber(delay) or 0), fn = fn, rep = rep, co = co }
  timers[#timers + 1] = t
  return t
end
local function tick_timers(dt)
  now = now + dt
  local due = {}
  for i = #timers, 1, -1 do
    local t = timers[i]
    if t.cancelled then table.remove(timers, i)
    elseif now >= t.at then due[#due + 1] = t; table.remove(timers, i) end
  end
  table.sort(due, function(a, b) return a.at < b.at end)
  for _, t in ipairs(due) do
    if t.co then
      arm()
      local ok, res = coroutine.resume(t.co)
      if not ok then report(t.mod, traceback(t.co, tostring(res)))
      elseif coroutine.status(t.co) ~= "dead" then add(t.mod, res, nil, nil, t.co) end
    else
      if call(t.mod, t.fn) and t.rep and not t.cancelled then t.at = now + t.rep; timers[#timers + 1] = t end
    end
  end
end


local level, class, actor, fullname, importtext, player = API.level, API.class, API.actor, API.fullname, API.importtext, API.player
local copyprops, copyprop, classof, isa = API.copyprops, API.copyprop, API.classof, API.isa
local all_actors, valid, nameof = API.actors, API.valid, API.nameof

-- Level changes: Mods\<mod>\Levels\<map>.txt, applied when a mission starts (not when a savegame loads: the save
-- already has them). See Mods\LEVELS.txt.
local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

local function args_of(rest)
  local a = {}
  for k, v in rest:gmatch("([%w_]+)%s*=%s*([^%s;]+)") do a[k:lower()] = v end
  return a
end

local function parse_level(text, report)
  local blocks, cur, ln = {}, nil, 0
  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    ln = ln + 1
    local s = trim(line:gsub("\r", ""))
    -- a comment is a whole line starting with ; // or #, or " ;" / " //" after a value
    s = trim((s:gsub("%s+;.*$", ""):gsub("%s+//.*$", "")))
    if s == "" or s:find("^;") or s:find("^//") or s:find("^#") then
      -- nothing
    elseif cur then
      if s:lower():find("^end[%s]") or s:lower() == "end" then
        blocks[#blocks + 1] = cur
        cur = nil
      else
        local k, v = s:match("^([%w_]+%s*%(?%s*%d*%s*%)?)%s*=%s*(.-)$")
        if k then cur.props[#cur.props + 1] = { key = (k:gsub("%s", "")), value = v, line = ln }
        else report(ln, "expected Property=Value or End: " .. s) end
      end
    else
      local kind, rest = s:match("^[Bb][Ee][Gg][Ii][Nn]%s+(%a+)%s*(.*)$")
      if kind then
        cur = { kind = kind:lower(), args = args_of(rest), props = {}, line = ln, head = s }
      else
        local drest = s:match("^[Dd][Ee][Ll][Ee][Tt][Ee]%s*(.*)$")
        if drest then blocks[#blocks + 1] = { kind = "delete", args = args_of(drest), props = {}, line = ln, head = s }
        else report(ln, "expected Begin Actor, Begin Change or Delete: " .. s) end
      end
    end
  end
  if cur then report(cur.line, "this Begin has no End") end
  return blocks
end

local function numbers(s)
  local t = {}
  for k, v in s:gmatch("([%a_]+)%s*=%s*([%-%+%d%.eE]+)") do t[k:upper()] = tonumber(v) end
  return t
end
local function to_vec(s) local t = numbers(s); return vec(t.X or 0, t.Y or 0, t.Z or 0) end
local function to_rot(s) local t = numbers(s); return { Pitch = math.floor(t.PITCH or 0), Yaw = math.floor(t.YAW or 0), Roll = math.floor(t.ROLL or 0) } end

-- Properties that have to go through the game's own functions (so collision, rendering and physics notice).
local SETTERS = { drawtype = "SetDrawType", staticmesh = "SetStaticMesh", drawscale = "SetDrawScale",
                  drawscale3d = "SetDrawScale3D", physics = "SetPhysics" }
local COLLISION = { bcollideactors = 1, bblockactors = 2, bblockplayers = 3 }
local SIZE = { collisionradius = 1, collisionheight = 2 }

local function apply_props(obj, block, labels, report, isChange)
  local coll, size
  for _, p in ipairs(block.props) do
    local ok, err = pcall(function()
      local key, idx = p.key:match("^([%w_]+)%((%d+)%)$")
      key = key or p.key
      idx = tonumber(idx) or 0
      local lk = key:lower()
      local value = p.value:gsub("@([%w_]+)", function(label)
        local o = labels[label]
        if not o then error("no actor labelled @" .. label .. " in this file", 0) end
        local cls, path = fullname(o):match("^(%S+) (.+)$")
        return cls .. "'" .. path .. "'"
      end)
      if lk == "location" then
        if isChange then obj:SetLocation(to_vec(value)) end
      elseif lk == "rotation" then
        if isChange then obj:SetRotation(to_rot(value)) end
      elseif COLLISION[lk] then
        coll = coll or { obj.bCollideActors, obj.bBlockActors, obj.bBlockPlayers }
        local v = value:lower()
        coll[COLLISION[lk]] = (v == "true" or v == "1")
      elseif SIZE[lk] then
        size = size or { obj.CollisionRadius, obj.CollisionHeight }
        size[SIZE[lk]] = tonumber(value) or error("needs a number", 0)
      elseif SETTERS[lk] then
        local old = obj[key]
        importtext(obj, key, value, idx)
        local new = obj[key]
        obj[key] = old
        obj[SETTERS[lk]](obj, new)
      else
        importtext(obj, key, value, idx)
      end
    end)
    if not ok then report(p.line, p.key .. ": " .. tostring(err)) end
  end
  if coll then
    local ok, err = pcall(function() obj:SetCollision(coll[1], coll[2], coll[3]) end)
    if not ok then report(block.line, "collision: " .. tostring(err)) end
  end
  if size then
    local ok, err = pcall(function() obj:SetCollisionSize(size[1], size[2]) end)
    if not ok then report(block.line, "collision size: " .. tostring(err)) end
  end
end

-- Diagnostics for copied characters (they didn't fall like ragdolls when killed): what differs from the original right
-- after the first copy of each class, and the physics state when a copy or its original dies.
local ED = "Level editor"
local copies = setmetatable({}, { __mode = "k" })
local diffed = {}
local function physics_line(p)
  local t = {}
  for _, n in ipairs({ "Health", "Physics", "HavokData", "HavokRigidBody", "HavokPhantom", "ColFlagHavok", "bUsePhysic",
                       "BasicPhysics", "AInertTrans", "bCollideActors", "Mesh" }) do
    local ok, v = pcall(function() return p[n] end)
    t[#t + 1] = n .. "=" .. (ok and tostring(v) or "?")
  end
  local ok, st = pcall(function() return p:GetStateName() end)
  return "state " .. (ok and tostring(st) or "?") .. ", " .. table.concat(t, " ")
end
local function safe_name(o) local ok, n = pcall(nameof, o); return ok and n or "?" end
local function death_log(what, p)
  core.log(ED, what .. " died: " .. physics_line(p))
  add(ED, 1.5, function() if valid(p) then core.log(ED, what .. ", 1.5 s later: " .. physics_line(p)) end end)
end
local function watch_copy(a, template)
  copies[a] = { template = template }
  local cn = safe_name(classof(a))
  if diffed[cn] then return end
  diffed[cn] = true
  local ok, d = pcall(core.diffprops, a, template)
  if not ok then return end
  core.log(ED, string.format("copied %s from %s: %d propert%s differ%s", safe_name(a), safe_name(template), #d,
    #d == 1 and "y" or "ies", #d > 0 and " (copy | original):" or ""))
  for i = 1, math.min(#d, 80) do core.log(ED, "  " .. d[i]) end
end
local function watch_copies()
  for c, info in pairs(copies) do
    if not valid(c) then copies[c] = nil
    else
      if not info.dead then
        local ok, h = pcall(function() return c.Health end)
        if ok and h <= 0 then info.dead = true; death_log("copy " .. safe_name(c), c) end
      end
      local t = info.template
      if not info.tdead and valid(t) then
        local ok, h = pcall(function() return t.Health end)
        if ok and h <= 0 then info.tdead = true; death_log("original " .. safe_name(t), t) end
      end
    end
  end
end

-- Deleting: an actor goes with what belongs to it - what it owns or carries (a guard's gun, flashlight and gear, and
-- what's attached to those). Characters and controllers are never taken along (Sam standing on a crate isn't the
-- crate's), and a character's AI goes with it anyway (Pawn.Destroyed destroys its controller). An actor the game won't
-- destroy is hidden and loses its collision instead; a character that stays (hidden) loses its AI too.
local function vanish(o, hide)
  local ok, done = pcall(function() return o:Destroy() end)
  if ok and done then return true end
  if hide then
    pcall(function() o.bHidden = true end)
    pcall(function() o:SetCollision(false, false, false) end)
  end
  return false
end
local function belongings_map()
  local kids = {}
  for _, a in ipairs(all_actors()) do
    if not isa(a, "Pawn") and not isa(a, "Controller") then
      for _, f in ipairs({ "Owner", "Base" }) do
        local ok, p = pcall(function() return a[f] end)
        if ok and p and p ~= a then
          local l = kids[p]
          if not l then l = {}; kids[p] = l end
          l[#l + 1] = { a = a, owned = (f == "Owner") }
        end
      end
    end
  end
  return kids
end
local function remove_actor(obj, kids)
  if obj == player() then error("that's Sam", 0) end
  if isa(obj, "Controller") and obj.bIsPlayer then error("that's Sam's controller", 0) end
  kids = kids or belongings_map()
  local list, seen = {}, { [obj] = true }
  local function collect(o)
    for _, k in ipairs(kids[o] or {}) do
      if not seen[k.a] then seen[k.a] = true; collect(k.a); list[#list + 1] = k end
    end
  end
  collect(obj)
  local ctrl
  if isa(obj, "Pawn") then pcall(function() ctrl = obj.Controller end) end
  local n = 0
  for _, k in ipairs(list) do if valid(k.a) and (vanish(k.a, k.owned) or k.owned) then n = n + 1 end end
  local done = vanish(obj, true)
  if not done and ctrl and valid(ctrl) then vanish(ctrl, false) end
  return done, n
end

-- A copy of 'template' at loc/rot: same class, the designer-set properties, and (for a character) a controller
-- like the original's. Used by Begin Actor Copy= and by the level editor's Duplicate. Errors on failure.
local function make_copy(template, loc, rot, warn)
  local a = level():Spawn(classof(template), nil, nil, loc, rot or template.Rotation)
  if not a then error("the game refused to spawn a copy there (blocked?)", 0) end
  copyprops(a, template)
  a.bHidden = false
  -- a character far from Sam has its body streamed out (Mesh empty, kept in BackupMesh): give the copy its body
  pcall(function() if a.Mesh == nil and a.BackupMesh ~= nil then a.Mesh = a.BackupMesh end end)
  -- what the copied values need from the game's own functions
  pcall(function() a:SetCollisionSize(a.CollisionRadius, a.CollisionHeight) end)
  pcall(function() local ds = a.DrawScale; a.DrawScale = 1.0; a:SetDrawScale(ds) end)
  if isa(a, "Pawn") then
    -- the animation set is linked to the body when the character spawns, which the copy didn't have yet
    pcall(function() a:LinkNPCAnims() end)
    pcall(function() a:SetupSkelMesh() end)
    -- its Havok shape (what it falls with when knocked out or killed)
    pcall(function() a:CreateHavokShape() end)
    -- a controller like the original's (the game only makes them at level start)
    local tc = template.Controller
    if tc then
      local c = level():Spawn(classof(tc), nil, nil, loc, rot or template.Rotation)
      if c then
        c:Possess(a)
        -- the AI's list of enemies (Sam) is only filled at level start: the original's controller has it
        for _, n in ipairs({ "EnemyInfo", "iEnemy", "iOtherEnemy", "iEnemyUpdateIndex" }) do pcall(copyprop, c, tc, n) end
      elseif warn then
        warn("couldn't create a controller for the copy")
      end
    end
    watch_copy(a, template)
  end
  return a
end

-- The level editor (editor.cpp) calls these.
function __ed_copy(template, x, y, z)
  return make_copy(template, vec(x, y, z), nil, nil)
end
function __ed_delete(obj)
  return remove_actor(obj)
end

local function apply_level(modname, folder, mapname, text)
  local problems = 0
  local function report(ln, msg)
    problems = problems + 1
    core.log(modname, "Levels\\" .. mapname .. ".txt line " .. ln .. ": " .. msg)
  end
  local blocks = parse_level(text, report)
  local labels, objs = {}, {}
  local added, changed, deleted = 0, 0, 0
  local lvl = level()
  -- 1. spawn the new actors (so @labels work in any order)
  for _, b in ipairs(blocks) do
    if b.kind == "actor" then
      local ok, err = pcall(function()
        local template
        if b.args.copy then
          template = labels[b.args.copy] or actor(b.args.copy) or error("no actor named " .. b.args.copy .. " to copy", 0)
        end
        local cname = b.args.class or (template and fullname(classof(template)):match("%S+ %S-%.?([%w_]+)$")) or error("Begin Actor needs Class= or Copy=", 0)
        local cls = (template and not b.args.class) and classof(template) or class(cname) or error("no class named " .. cname, 0)
        local loc, rot = player().Location, { Pitch = 0, Yaw = 0, Roll = 0 }
        if template then rot = template.Rotation end   -- (not its position: the copy would spawn inside it)
        for _, p in ipairs(b.props) do
          local lk = p.key:lower()
          if lk == "location" then loc = to_vec(p.value) elseif lk == "rotation" then rot = to_rot(p.value) end
        end
        local a
        if template then
          a = make_copy(template, loc, rot, function(m) report(b.line, m) end)
        else
          a = lvl:Spawn(cls, nil, nil, loc, rot)
          if not a then error("the game refused to spawn a " .. cname .. " there (blocked, or a class that can't be added while playing)", 0) end
        end
        objs[b] = a
        if b.args.name then labels[b.args.name] = a end
        added = added + 1
      end)
      if not ok then report(b.line, tostring(err)) end
    elseif b.kind == "change" then
      local target = b.args.name and (labels[b.args.name] or actor(b.args.name))
      if target then
        objs[b] = target
        if b.args.label then labels[b.args.label] = target end
      else report(b.line, "no actor named " .. tostring(b.args.name) .. " in this level") end
    elseif b.kind ~= "delete" then
      report(b.line, "unknown block 'Begin " .. b.kind .. "' (Actor or Change)")
    end
  end
  -- 2. properties
  for _, b in ipairs(blocks) do
    local o = objs[b]
    if o then
      apply_props(o, b, labels, report, b.kind == "change")
      if b.kind == "change" then changed = changed + 1 end
    end
  end
  -- 3. deletions, with what belongs to them (a guard's gun...); an actor the game won't destroy is hidden instead
  local kids
  for _, b in ipairs(blocks) do
    if b.kind == "delete" then
      local ok, err = pcall(function()
        local target = b.args.name and (labels[b.args.name] or actor(b.args.name))
        if not target then error("no actor named " .. tostring(b.args.name) .. " in this level", 0) end
        kids = kids or belongings_map()
        remove_actor(target, kids)
        deleted = deleted + 1
      end)
      if not ok then report(b.line, tostring(err)) end
    end
  end
  -- 4. what this file did, for the level editor (its Save writes the file back with the editor's changes merged in)
  local recs = {}
  for _, b in ipairs(blocks) do
    local r = { kind = b.kind, head = b.head, obj = objs[b], props = {} }
    if b.kind == "actor" then r.label = b.args.name; r.copy = b.args.copy; r.class = b.args.class
    elseif b.kind == "change" then r.label = b.args.label; r.name = b.args.name
    elseif b.kind == "delete" then r.name = b.args.name end
    for _, p in ipairs(b.props) do r.props[#r.props + 1] = { p.key, p.value } end
    recs[#recs + 1] = r
  end
  pcall(core.record, folder, recs)
  core.log(modname, string.format("%s: %d actor(s) added, %d changed, %d deleted%s", mapname, added, changed, deleted,
    problems > 0 and (", " .. problems .. " problem(s) above") or ""))
end

local function apply_levels(mapname)
  for i = 0, core.modcount() - 1 do
    local text = core.levelfile(i, mapname)
    if text then
      arm()
      local ok, err = xpcall(apply_level, traceback, core.modname(i), core.modfolder(i), mapname, text)
      if not ok then core.log(core.modname(i), "ERROR applying level changes: " .. tostring(err)) end
    end
  end
end

local valid_events = { map = true, level = true, tick = true }

function __newmod(index, name)
  local env = {}
  local loaded = {}
  local proxy = setmetatable(env, { __index = API, __metatable = false })
  env._G = env
  env.print = function(...) core.log(name, ...) end
  env.on = function(event, fn)
    assert(valid_events[event], "unknown event '" .. tostring(event) .. "' (map, level or tick)")
    assert(type(fn) == "function", "on() needs a function")
    local list = handlers[event] or {}
    handlers[event] = list
    list[#list + 1] = { mod = name, fn = fn, errors = 0 }
  end
  env.after = function(s, fn) return add(name, s, fn) end
  env.every = function(s, fn) return add(name, s, fn, math.max(tonumber(s) or 0, 0.001)) end
  env.cancel = function(t) if t then t.cancelled = true end end
  env.run = function(fn)
    local co = coroutine.create(fn)
    local ok, res = coroutine.resume(co)
    if not ok then error(res, 2) end
    if coroutine.status(co) ~= "dead" then return add(name, res, nil, nil, co) end
  end
  env.wait = function(s) coroutine.yield(tonumber(s) or 0) end
  env.load = function(chunk, cname, mode, e) return load(chunk, cname, "t", e or env) end
  env.require = function(module)
    if loaded[module] ~= nil then return loaded[module] end
    local fn, err = core.load(index, (module:gsub("%.", "\\")) .. ".lua", env)
    if not fn then error(err, 2) end
    local res = fn()
    if res == nil then res = true end
    loaded[module] = res
    return res
  end
  mods[index] = { name = name, env = env }
  return env
end

function __runmod(index)
  local m = mods[index]
  local count = 0
  for _, file in ipairs(core.scripts(index)) do
    local fn, err = core.load(index, file, m.env)
    if fn then
      if call(m.name, fn) then count = count + 1 end
    else
      report(m.name, err)
    end
  end
  return count
end

function __fire(event, ...)
  if event == "tick" then
    tick_timers((...)); watch_copies()
    local ok, err = pcall(plush_tick)
    if not ok and not plush.failed then plush.failed = true; core.log("Niko plush", "ERROR: " .. tostring(err)) end
  end
  if event == "level" and core.leveldue() then apply_levels((...)) end
  local list = handlers[event]
  if not list then return end
  for _, h in ipairs(list) do
    if not h.off then
      if call(h.mod, h.fn, ...) then
        h.errors = 0
      else
        h.errors = h.errors + 1
        if h.errors >= 10 then
          h.off = true
          core.log(h.mod, "a '" .. event .. "' handler failed 10 times in a row and was switched off")
        end
      end
    end
  end
end
)LUA";

    // Runs fn on the Lua stack top with nargs arguments under the time limit; logs failures.
    bool Protected(int nargs, int nresults)
    {
        const bool outer = g_depth++ == 0;
        if (outer)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            g_deadline.QuadPart = now.QuadPart + g_freq.QuadPart;   // 1 s
        }
        const int base = lua_gettop(L) - nargs;
        lua_pushcfunction(L, &L_traceback);
        lua_insert(L, base);
        const int r = lua_pcall(L, nargs, nresults, base);
        lua_remove(L, base);
        if (outer) g_deadline.QuadPart = 0;
        --g_depth;
        if (r != LUA_OK)
        {
            LOG("[LUA] ERROR: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    void Fire(const char* event, const std::vector<std::string>& args, double number = 0, bool withNumber = false)
    {
        if (!L) return;
        lua_getglobal(L, "__fire");
        lua_pushstring(L, event);
        int n = 1;
        if (withNumber) { lua_pushnumber(L, number); ++n; }
        for (const auto& a : args) { lua_pushstring(L, a.c_str()); ++n; }
        Protected(n, 0);
    }

    void Close()
    {
        if (L) lua_close(L);
        L = nullptr;
        g_props.clear();
        g_findCache.clear();
    }

    void Open()
    {
        Close();
        g_scriptCount = 0;
        const auto& mods = Mods::Loaded();
        bool any = false;
        for (const auto& m : mods)
            if ((g_settings.scripts && GetFileAttributesW((m.dir + L"Scripts").c_str()) != INVALID_FILE_ATTRIBUTES) ||
                (g_settings.levelChanges && GetFileAttributesW((m.dir + L"Levels").c_str()) != INVALID_FILE_ATTRIBUTES))
                any = true;
        if (!any && g_settings.editorKey <= 0 && !Plush::Available()) return;   // the editor and the plush use Lua too

        L = luaL_newstate();
        if (!L) { LOG("[LUA] couldn't create the Lua state"); return; }
        // Safe standard libraries only (no io, os.execute, debug, package).
        const luaL_Reg libs[] = { { LUA_GNAME, luaopen_base }, { LUA_COLIBNAME, luaopen_coroutine }, { LUA_TABLIBNAME, luaopen_table },
                                  { LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 },
                                  { LUA_OSLIBNAME, luaopen_os } };
        for (const auto& lib : libs) { luaL_requiref(L, lib.name, lib.func, 1); lua_pop(L, 1); }
        // os: only the clock and date functions; base: no file loading; string: no bytecode dumps.
        lua_getglobal(L, "os");
        for (const char* f : { "execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale" }) { lua_pushnil(L); lua_setfield(L, -2, f); }
        lua_pop(L, 1);
        for (const char* f : { "dofile", "loadfile" }) { lua_pushnil(L); lua_setglobal(L, f); }
        lua_getglobal(L, "string"); lua_pushnil(L); lua_setfield(L, -2, "dump"); lua_pop(L, 1);
        lua_sethook(L, &TimeHook, LUA_MASKCOUNT, 10000);

        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "SCCT.objects");   // weak cache: pointer -> userdata
        lua_getfield(L, LUA_REGISTRYINDEX, "SCCT.objects");
        lua_newtable(L);
        lua_pushstring(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pop(L, 1);

        lua_newtable(L);
        lua_pushcfunction(L, &StructIndex);
        lua_setfield(L, -2, "__index");
        lua_setfield(L, LUA_REGISTRYINDEX, "SCCT.struct");

        luaL_newmetatable(L, kObjMeta);
        const luaL_Reg meta[] = { { "__index", &ObjIndex }, { "__newindex", &ObjNewIndex }, { "__tostring", &ObjToString },
                                  { "__eq", &ObjEq }, { nullptr, nullptr } };
        luaL_setfuncs(L, meta, 0);
        lua_pushstring(L, "object");
        lua_setfield(L, -2, "__name");
        lua_pop(L, 1);

        // core (internal) and API (what mods see, besides their own globals).
        lua_newtable(L);
        const luaL_Reg core[] = { { "log", &L_log }, { "traceback", &L_traceback }, { "load", &L_load }, { "scripts", &L_scripts },
                                  { "setvector", &L_setvector }, { "arm", &L_arm }, { "modcount", &L_modcount }, { "modname", &L_modname },
                                  { "levelfile", &L_levelfile }, { "leveldue", &L_leveldue }, { "modfolder", &L_modfolder },
                                  { "record", &L_record }, { "diffprops", &L_diffprops }, { "plush_on", &L_plush_on },
                                  { "plush_draw", &L_plush_draw }, { "plush_meow", &L_plush_meow }, { "plush_noise", &L_plush_noise },
                                  { "plush_quiet", &L_plush_quiet }, { "plush_camdist", &L_plush_camdist },
                                  { nullptr, nullptr } };
        luaL_setfuncs(L, core, 0);
        lua_newtable(L);
        const luaL_Reg api[] = { { "player", &L_player }, { "level", &L_level }, { "map", &L_map }, { "time", &L_time },
                                 { "actors", &L_actors }, { "objects", &L_objects }, { "class", &L_class }, { "valid", &L_valid },
                                 { "isa", &L_isa }, { "nameof", &L_nameof }, { "classof", &L_classof }, { "fullname", &L_fullname },
                                 { "dump", &L_dump }, { "key", &L_key }, { "pressed", &L_pressed }, { "importtext", &L_importtext },
                                 { "actor", &L_actor }, { "copyprops", &L_copyprops }, { "copyprop", &L_copyprop }, { nullptr, nullptr } };
        luaL_setfuncs(L, api, 0);
        for (const char* g : { "assert", "error", "ipairs", "pairs", "next", "pcall", "xpcall", "select", "tonumber", "tostring",
                               "type", "rawequal", "rawget", "rawset", "rawlen", "setmetatable", "getmetatable", "collectgarbage",
                               "string", "table", "math", "utf8", "coroutine", "os" })
        {
            lua_getglobal(L, g);
            lua_setfield(L, -2, g);
        }
        lua_newtable(L);
        lua_setfield(L, -2, "shared");

        if (luaL_loadbufferx(L, kPrelude, strlen(kPrelude), "=prelude", "t") != LUA_OK)
        {
            LOG("[LUA] prelude: %s", lua_tostring(L, -1));
            Close();
            return;
        }
        lua_insert(L, -3);   // prelude, core, API
        if (!Protected(2, 0)) { Close(); return; }

        for (size_t i = 0; i < mods.size() && g_settings.scripts; ++i)
        {
            if (GetFileAttributesW((mods[i].dir + L"Scripts").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const std::string name = Utf8(mods[i].name.c_str(), mods[i].name.size());
            lua_getglobal(L, "__newmod");
            lua_pushinteger(L, lua_Integer(i));
            lua_pushstring(L, name.c_str());
            if (!Protected(2, 0)) continue;
            lua_getglobal(L, "__runmod");
            lua_pushinteger(L, lua_Integer(i));
            if (Protected(1, 1))
            {
                const int n = int(lua_tointeger(L, -1));
                lua_pop(L, 1);
                g_scriptCount += n;
                LOG("[LUA] %s: %d script(s) loaded", name.c_str(), n);
            }
        }
    }
}

void Scripting::Init()
{
    QueryPerformanceFrequency(&g_freq);
    if (!g_settings.mods && !Plush::Available())
    {
        LOG("Scripting: off");
        return;
    }
    // The engine lookups are also used to name texture dumps, so they're made even with scripts off.
    if (const uintptr_t a = Mem::Find("object list", "8B 41 04 40 89 41 04 3B 05 ? ? ? ? 7D ? 8B 15 ? ? ? ? 8B 04 82 85 C0"))
    {
        g_objNum = *reinterpret_cast<int**>(a + 9);
        g_objData = *reinterpret_cast<void****>(a + 17);
    }
    pStrAssign = reinterpret_cast<StrAssign_t>(Mem::Find("FString assign",
        "53 8B D9 8B 03 56 8B 74 24 0C 3B C6 74 4D 66 83 3E 00 74 0C 56 E8 ? ? ? ? 83 C4 04 40 EB 02 33 C0 89 43 08 89 43 04 "
        "8B 0D ? ? ? ? 8B 11 68 ? ? ? ? 03 C0 50 8B 03 50 FF 52 04", 0, 2));
    if (!g_settings.scripts && !g_settings.levelChanges && g_settings.editorKey <= 0 && !Plush::Available())
    {
        LOG("Scripting: off ([Mods] Scripts = 0, LevelChanges = 0, no editor)");
        return;
    }
    if (!g_objData || !pStrAssign || !Script::Ready())
    {
        LOG("Scripting: unavailable (engine patterns not found)");
        return;
    }
    Open();
    LOG("Scripting: %s", L ? "Lua 5.4 ready" : "no mod has a Scripts or Levels folder");
}

namespace
{
    // True when [p, p+n) is committed, readable memory without a guard page (so probing a stack value as a pointer can't
    // trip another thread's stack guard).
    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000 || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & 0xEE)) return false;
        return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
}

bool Scripting::TextureName(const void* from, size_t bytes, unsigned w, unsigned h, std::string& out)
{
    if (!g_objData || !Script::Ready()) return false;
    static void* texClass = nullptr;
    static int uName = 0, vName = 0;
    if (!texClass) texClass = FindClass("Texture");
    if (!uName) { uName = NameIdx("USize"); vName = NameIdx("VSize"); }
    if (!texClass || !uName || !vName) return false;
    static uintptr_t exeLo = 0, exeHi = 0;
    if (!exeLo)
    {
        const auto* mz = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mz + reinterpret_cast<const IMAGE_DOS_HEADER*>(mz)->e_lfanew);
        exeLo = reinterpret_cast<uintptr_t>(mz);
        exeHi = exeLo + nt->OptionalHeader.SizeOfImage;
    }
    const auto* p = static_cast<const uint8_t*>(from);
    for (size_t off = 0; off + 4 <= bytes; off += 4)
    {
        if (!Readable(p + off, 4)) break;
        void* v = *reinterpret_cast<void* const*>(p + off);
        // The UTexture itself, an FStaticTexture-like render interface pointing at it (+4/+8), or, for a C++ object with
        // a vtable in the exe, a render interface embedded in the UTexture: the object then starts a little before it.
        std::vector<void*> cands = { v };
        if (Readable(v, 12))
        {
            cands.push_back(Rd<void*>(v, 4));
            cands.push_back(Rd<void*>(v, 8));
            const uintptr_t vt = Rd<uintptr_t>(v, 0);
            const uintptr_t at = reinterpret_cast<uintptr_t>(v);
            if (vt >= exeLo && vt < exeHi && at > 0x1000 && Readable(reinterpret_cast<void*>(at - 0x400), 0x400))
                for (uintptr_t k = 4; k <= 0x400; k += 4) cands.push_back(reinterpret_cast<void*>(at - k));
        }
        for (void* c : cands)
        {
            if (!c || !Readable(c, 0x28) || !Alive(c) || !IsA(c, texClass)) continue;
            void* cls = ClassOf(c);
            void* up = FindProp(cls, uName);
            void* vp = FindProp(cls, vName);
            if (!up || !vp) continue;
            const int us = Rd<int>(c, Describe(up).offset), vs = Rd<int>(c, Describe(vp).offset);
            for (int k = 0; k < 8; ++k)
                if (us > 0 && vs > 0 && unsigned(std::max(1, us >> k)) == w && unsigned(std::max(1, vs >> k)) == h)
                {
                    std::string name = FullName(c);
                    const size_t space = name.find(' ');
                    out = space == std::string::npos ? name : name.substr(space + 1);
                    return !out.empty();
                }
        }
    }
    return false;
}

void Scripting::OnMapLoad(const wchar_t* map, const wchar_t* options)
{
    std::wstring m = map ? map : L"";
    if (const size_t slash = m.find_last_of(L"\\/"); slash != std::wstring::npos) m.erase(0, slash + 1);
    if (const size_t dot = m.find_last_of(L'.'); dot != std::wstring::npos) m.resize(dot);
    g_map = m;
    g_pawn = nullptr;
    g_levelPending = true;
    {
        std::wstring o = options ? options : L"";
        for (auto& c : o) c = towupper(c);
        g_levelDue = o.find(L"MISSIONDIFF") != std::wstring::npos && _wcsicmp(m.c_str(), L"menu") != 0;
    }
    Editor::OnMapLoad();
    if (!L) return;
    const std::wstring o = options ? options : L"";
    Fire("map", { Utf8(m.c_str(), m.size()), Utf8(o.c_str(), o.size()) });
}

namespace
{
    // Ctrl + LevelDumpKey: every actor of the level (name, class, position, tag, static mesh) to Mods\_LevelDump\<map>.txt.
    void DumpLevel(void* pawn)
    {
        void* xlevel = Rd<void*>(pawn, kActorXLevel);
        void** actors = xlevel ? Rd<void**>(xlevel, kXLevelActors) : nullptr;
        const int count = xlevel ? std::clamp(Rd<int>(xlevel, kXLevelCount), 0, 200000) : 0;
        if (!actors || g_map.empty()) return;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir.resize(dir.find_last_of(L'\\'));
        dir.resize(dir.find_last_of(L'\\') + 1);
        dir += L"Mods\\_LevelDump\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + g_map + L".txt";
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) { LOG("[LEVEL] couldn't write %ls", path.c_str()); return; }
        fprintf(f, "; Every actor in %s: name, class, location (X, Y, Z), tag and static mesh. Use the names in\r\n"
                   "; Levels\\%s.txt (Begin Change Name=... / Delete Name=...). See Mods\\LEVELS.txt.\r\n",
                Utf8(g_map.c_str(), g_map.size()).c_str(), Utf8(g_map.c_str(), g_map.size()).c_str());
        const int locName = NameIdx("Location"), tagName = NameIdx("Tag"), meshName = NameIdx("StaticMesh");
        int n = 0;
        for (int i = 0; i < count; ++i)
        {
            void* a = Rd<void*>(actors, uintptr_t(i) * 4);
            if (!a || (Rd<uint32_t>(a, kActorFlags) & kDeleteMe)) continue;
            void* cls = ClassOf(a);
            float loc[3] = {};
            if (void* lp = locName ? FindProp(cls, locName) : nullptr) SafeRead(static_cast<uint8_t*>(a) + Describe(lp).offset, loc, 12);
            std::string tag, mesh;
            if (void* tp = tagName ? FindProp(cls, tagName) : nullptr)
            {
                const int t = Rd<int>(a, Describe(tp).offset);
                if (t && t != NameOf(cls)) tag = NameStr(t);
            }
            if (void* mp = meshName ? FindProp(cls, meshName) : nullptr)
                if (void* m = Rd<void*>(a, Describe(mp).offset); m && Alive(m))
                {
                    std::string full = FullName(m);
                    const size_t sp = full.find(' ');
                    mesh = sp == std::string::npos ? full : full.substr(sp + 1);
                }
            fprintf(f, "%-40s %-32s (%.0f, %.0f, %.0f)%s%s%s%s\r\n", NameStr(NameOf(a)).c_str(), NameStr(NameOf(cls)).c_str(),
                    loc[0], loc[1], loc[2], tag.empty() ? "" : "  Tag=", tag.c_str(), mesh.empty() ? "" : "  StaticMesh=", mesh.c_str());
            ++n;
        }
        fclose(f);
        LOG("[LEVEL] %d actors listed in %ls", n, path.c_str());
    }
}

void Scripting::OnPlayerTick(void* pawn, float dt)
{
    if (!L && !g_settings.scripts) return;
    g_pawn = pawn;
    if (L) ReadKeys();

    // Ctrl + ReloadScriptsKey: reload every script (the level event fires again for the current map).
    if (g_settings.reloadScriptsKey > 0 && g_settings.reloadScriptsKey < 256 && Game::hWnd && GetForegroundWindow() == Game::hWnd)
    {
        const bool down = (GetAsyncKeyState(g_settings.reloadScriptsKey) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (down && !g_reloadHeld && g_objData && pStrAssign)
        {
            LOG("[LUA] reloading scripts");
            Open();
            LOG("[LUA] %d script(s) loaded", g_scriptCount);
            g_levelPending = true;
        }
        g_reloadHeld = down;
        const int dumpKey = g_settings.levelDumpKey;
        const bool dump = dumpKey > 0 && dumpKey < 256 && (GetAsyncKeyState(dumpKey) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (dump && !g_levelDumpHeld && g_objData) DumpLevel(pawn);
        g_levelDumpHeld = dump;
    }
    if (!L) return;
    if (g_levelPending)
    {
        g_levelPending = false;
        Fire("level", { Utf8(g_map.c_str(), g_map.size()) });
    }
    Fire("tick", {}, dt, true);
}

// ===== Engine access for the level editor ==========================================================================
// The editor (editor.cpp) reads and writes the same objects as Lua, through these. Values are shown and entered in
// Unreal text syntax: UProperty::ExportText (vtable +90h, as execGetPropertyText calls it: Index, buffer, Data, Delta,
// PortFlags, with Data = Delta = the object so it always exports) and ImportText (+8Ch). Properties the game only
// takes through a function (Location, Rotation, DrawScale, StaticMesh, collision, ...) go through that function.
namespace
{
    using ExportText_t = int(__fastcall*)(void* prop, void* edx, int index, wchar_t* buf, uint8_t* data, uint8_t* delta, int flags);
    bool ExportRaw(void* prop, int index, wchar_t* buf, void* obj)
    {
        __try
        {
            buf[0] = 0;
            reinterpret_cast<ExportText_t>((*reinterpret_cast<void***>(prop))[0x90 / 4])(prop, nullptr, index, buf,
                static_cast<uint8_t*>(obj), static_cast<uint8_t*>(obj), 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; return false; }
    }

    // Calls a native setter on 'obj' with bytecode arguments (see Script::Args); false if it doesn't exist or failed.
    bool CallSetter(void* obj, const char* function, Script::Args& args)
    {
        const int name = NameIdx(function);
        void* fn = name ? Script::Function(obj, name) : nullptr;
        if (!fn) return false;
        const uint16_t iNative = Rd<uint16_t>(fn, kFuncNative);
        void* code = iNative ? Script::Native(iNative) : Rd<void*>(fn, kFuncCode);
        if (!code) return false;
        alignas(16) uint8_t result[64] = {};
        return Script::CallNativeEx(obj, code, args.End(), result);
    }

    bool PropBool(void* obj, const char* name)
    {
        const int n = NameIdx(name);
        void* p = n ? FindProp(ClassOf(obj), n) : nullptr;
        if (!p) return false;
        const Prop& d = Describe(p);
        return d.type == T::Bool && (Rd<uint32_t>(obj, d.offset) & d.mask) != 0;
    }
    float PropFloat(void* obj, const char* name)
    {
        const int n = NameIdx(name);
        void* p = n ? FindProp(ClassOf(obj), n) : nullptr;
        return p ? Rd<float>(obj, Describe(p).offset) : 0.0f;
    }

    void* g_savedPauser = nullptr;
    bool g_paused = false;
}

void* Engine::Player() { return Pawn(); }
bool Engine::Alive(void* obj) { return ::Alive(obj); }

void Engine::Actors(std::vector<void*>& out)
{
    out.clear();
    void* pawn = Pawn();
    void* xlevel = pawn ? Rd<void*>(pawn, kActorXLevel) : nullptr;
    void** actors = xlevel ? Rd<void**>(xlevel, kXLevelActors) : nullptr;
    const int count = xlevel ? std::clamp(Rd<int>(xlevel, kXLevelCount), 0, 200000) : 0;
    for (int i = 0; actors && i < count; ++i)
    {
        void* a = Rd<void*>(actors, uintptr_t(i) * 4);
        if (a && !(Rd<uint32_t>(a, kActorFlags) & kDeleteMe)) out.push_back(a);
    }
}

std::string Engine::Name(void* obj) { return obj ? NameStr(NameOf(obj)) : ""; }
std::string Engine::ClassName(void* obj) { return obj ? NameStr(NameOf(ClassOf(obj))) : ""; }
std::string Engine::FullName(void* obj) { return obj ? ::FullName(obj) : ""; }

bool Engine::Location(void* actor, float out[3])
{
    static int locName = 0;
    if (!locName) locName = NameIdx("Location");
    void* p = locName && actor ? FindProp(ClassOf(actor), locName) : nullptr;
    return p && SafeRead(static_cast<uint8_t*>(actor) + Describe(p).offset, out, 12);
}

void Engine::Properties(void* obj, std::vector<PropRow>& out)
{
    out.clear();
    if (!obj || !::Alive(obj)) return;
    std::unordered_set<int> seen;
    Fields(ClassOf(obj), true, [&](void* f) {
        const Prop& d = Describe(f);
        if (d.type == T::None || !seen.insert(d.name).second) return true;
        PropRow r;
        r.prop = f;
        r.name = NameStr(d.name);
        void* outer = Rd<void*>(f, 0x18);   // UObject::Outer: the class (or struct) declaring it
        r.owner = outer ? NameStr(NameOf(outer)) : "";
        r.type = TypeName(d.type);
        r.dim = d.dim;
        r.internal = d.offset < 0x28 || (d.flags & 0x404C00) != 0;
        r.editable = (d.flags & 0x1) != 0;
        out.push_back(r);
        return true;
    });
}

bool Engine::Export(void* obj, void* prop, int index, std::string& out)
{
    out.clear();
    if (!obj || !::Alive(obj) || !prop) return false;
    static std::vector<wchar_t> buf(32768);
    if (!ExportRaw(prop, index, buf.data(), obj)) return false;
    buf.back() = 0;
    out = Utf8(buf.data(), wcslen(buf.data()));
    return true;
}

bool Engine::Import(void* obj, void* prop, int index, const std::string& text, std::string& error)
{
    error.clear();
    if (!obj || !::Alive(obj) || !prop) { error = "the object no longer exists"; return false; }
    const Prop& d = Describe(prop);
    if (index < 0 || index >= d.dim) { error = "no such element"; return false; }
    if (d.offset < 0x28 || (d.flags & 0x404C00)) { error = "that's the engine's own bookkeeping"; return false; }
    uint8_t* addr = static_cast<uint8_t*>(obj) + d.offset + size_t(index) * d.size;
    std::string lname = NameStr(d.name);
    for (auto& c : lname) c = char(tolower(static_cast<unsigned char>(c)));
    static const std::unordered_set<std::string> kSetters = { "location", "rotation", "drawscale", "drawscale3d",
        "staticmesh", "drawtype", "physics", "bcollideactors", "bblockactors", "bblockplayers", "collisionradius",
        "collisionheight" };
    const bool viaSetter = kSetters.count(lname) && d.size <= 64;
    uint8_t before[64] = {}, after[64] = {};
    if (viaSetter) SafeRead(addr, before, d.size);
    const std::wstring w = Wide(text.data(), text.size());
    const wchar_t* r = ImportRaw(d.p, w.c_str(), addr);
    if (r == reinterpret_cast<const wchar_t*>(-1)) { error = "the game crashed reading it"; return false; }
    if (!r) { error = "couldn't read that as a " + std::string(TypeName(d.type)) + " value"; return false; }
    if (!viaSetter) return true;

    // Put the old value back and hand the new one to the game's own function.
    SafeRead(addr, after, d.size);
    SafeWrite(addr, before, d.size);
    Script::Args a;
    bool ok = true;
    if (lname == "location") { const uint8_t t = 0x23; a.Raw(&t, 1); a.Raw(after, 12); a.Skip(); ok = CallSetter(obj, "SetLocation", a); }
    else if (lname == "rotation") { const uint8_t t = 0x22; a.Raw(&t, 1); a.Raw(after, 12); ok = CallSetter(obj, "SetRotation", a); }
    else if (lname == "drawscale") { float v; memcpy(&v, after, 4); a.Float(v); ok = CallSetter(obj, "SetDrawScale", a); }
    else if (lname == "drawscale3d") { const uint8_t t = 0x23; a.Raw(&t, 1); a.Raw(after, 12); ok = CallSetter(obj, "SetDrawScale3D", a); }
    else if (lname == "staticmesh") { const uint8_t t = 0x20; a.Raw(&t, 1); a.Raw(after, 4); ok = CallSetter(obj, "SetStaticMesh", a); }
    else if (lname == "drawtype") { a.Byte(after[0]); ok = CallSetter(obj, "SetDrawType", a); }
    else if (lname == "physics") { a.Byte(after[0]); ok = CallSetter(obj, "SetPhysics", a); }
    else if (lname == "collisionradius" || lname == "collisionheight")
    {
        float v;
        memcpy(&v, after, 4);
        a.Float(lname == "collisionradius" ? v : PropFloat(obj, "CollisionRadius"));
        a.Float(lname == "collisionheight" ? v : PropFloat(obj, "CollisionHeight"));
        ok = CallSetter(obj, "SetCollisionSize", a);
    }
    else   // collision flags
    {
        uint32_t bits;
        memcpy(&bits, after, 4);
        const bool v = (bits & d.mask) != 0;
        a.Bool(lname == "bcollideactors" ? v : PropBool(obj, "bCollideActors"));
        a.Bool(lname == "bblockactors" ? v : PropBool(obj, "bBlockActors"));
        a.Bool(lname == "bblockplayers" ? v : PropBool(obj, "bBlockPlayers"));
        ok = CallSetter(obj, "SetCollision", a);
    }
    if (!ok) error = "the game's function for it failed";
    return ok;
}

// Freezes the world (LevelInfo.Pauser, like the pause menu) while the editor is open.
void Engine::SetPause(bool on)
{
    void* lvl = LevelInfo();
    void* pawn = Pawn();
    const int pauserName = NameIdx("Pauser");
    void* pp = lvl && pauserName ? FindProp(ClassOf(lvl), pauserName) : nullptr;
    if (!pp)
    {
        if (!on) { g_paused = false; g_savedPauser = nullptr; }   // the level is gone (map change): nothing to restore
        return;
    }
    const Prop& d = Describe(pp);
    void** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(lvl) + d.offset);
    if (on && !g_paused)
    {
        // whoever is allowed in Pauser: the player's controller, its PlayerReplicationInfo, or Sam
        void* cand[3] = { nullptr, nullptr, pawn };
        if (pawn)
        {
            const int cn = NameIdx("Controller");
            if (void* cp = cn ? FindProp(ClassOf(pawn), cn) : nullptr) cand[0] = Rd<void*>(pawn, Describe(cp).offset);
            const int pn = NameIdx("PlayerReplicationInfo");
            if (void* rp = cand[0] && pn ? FindProp(ClassOf(cand[0]), pn) : nullptr) cand[1] = Rd<void*>(cand[0], Describe(rp).offset);
        }
        for (void* c : cand)
            if (c && ::Alive(c) && (!d.ref || IsA(c, d.ref)))
            {
                g_savedPauser = Rd<void*>(slot, 0);
                if (!g_savedPauser) SafeWrite(slot, &c, 4);
                g_paused = true;
                return;
            }
    }
    else if (!on && g_paused)
    {
        SafeWrite(slot, &g_savedPauser, 4);
        g_savedPauser = nullptr;
        g_paused = false;
    }
}

void Scripting::Reload()
{
    if (!g_objData || !pStrAssign) return;
    LOG("[LUA] reloading scripts");
    Open();
    LOG("[LUA] %d script(s) loaded", g_scriptCount);
    g_levelPending = true;
}

void Scripting::DumpLevelNow() { if (void* p = Pawn()) DumpLevel(p); }

// ----- more engine access for the editor: names, parameters, tracing, and Lua's copy/delete -----
int Engine::NameIndex(const char* text) { return NameIdx(text); }

int Engine::ParamOffset(void* func, const char* name)
{
    const int n = NameIdx(name);
    int off = -1;
    if (!n || !func) return -1;
    Fields(func, false, [&](void* f) {
        if (NameOf(f) == n && Describe(f).type != T::None) { off = Describe(f).offset; return false; }
        return true;
    });
    return off;
}

float Engine::GetFloat(void* obj, const char* name, float def)
{
    const int n = NameIdx(name);
    void* p = obj && n ? FindProp(ClassOf(obj), n) : nullptr;
    if (!p || Describe(p).type != T::Float) return def;
    return Rd<float>(obj, Describe(p).offset, def);
}

void* Engine::GetObject(void* obj, const char* name)
{
    const int n = NameIdx(name);
    void* p = obj && n ? FindProp(ClassOf(obj), n) : nullptr;
    if (!p) return nullptr;
    const Prop& d = Describe(p);
    if (d.type != T::Object && d.type != T::Class) return nullptr;
    void* v = Rd<void*>(obj, d.offset);
    return v && ::Alive(v) ? v : nullptr;
}

// Actor.Trace from 'tracer' (which it ignores): the first actor hit between start and end (with bTraceActors), or the
// level when only geometry is hit; hit gets the spot. Arguments go in like CallFunction's: vectors as constants, out
// parameters as locals.
void* Engine::Trace(void* tracer, const float start[3], const float end[3], float hit[3])
{
    static int traceName = 0;
    if (!traceName) traceName = NameIdx("Trace");
    void* fn = tracer && traceName ? Script::Function(tracer, traceName) : nullptr;
    if (!fn) return nullptr;
    const uint16_t parmsSize = Rd<uint16_t>(fn, kFuncParms);
    std::vector<uint8_t> locals(size_t(parmsSize) + 64, 0);
    Script::Args args;
    int hitOffset = -1;
    const Prop* ret = nullptr;
    Fields(fn, false, [&](void* f) {
        const Prop& d = Describe(f);
        if (d.type == T::None || !(d.flags & CPF_Parm) || d.offset >= parmsSize) return true;
        if (d.flags & CPF_ReturnParm) { ret = &d; return true; }
        std::string n = NameStr(d.name);
        for (auto& c : n) c = char(tolower(static_cast<unsigned char>(c)));
        if (d.flags & CPF_OutParm)
        {
            if (n == "hitlocation") hitOffset = d.offset;
            const uint8_t tok = 0x00;   // EX_LocalVariable
            void* p = d.p;
            args.Raw(&tok, 1);
            args.Raw(&p, 4);
        }
        else if (n == "traceend" || n == "tracestart")
        {
            const uint8_t tok = 0x23;   // EX_VectorConst
            args.Raw(&tok, 1);
            args.Raw(n == "traceend" ? end : start, 12);
        }
        else if (n == "btraceactors") args.Bool(true);
        else args.Skip();
        return true;
    });
    const uint16_t iNative = Rd<uint16_t>(fn, kFuncNative);
    void* code = iNative ? Script::Native(iNative) : Rd<void*>(fn, kFuncCode);
    alignas(16) uint8_t result[64] = {};
    if (!code || !Script::CallNativeEx(tracer, code, args.End(), result, locals.data())) return nullptr;
    if (hitOffset >= 0) memcpy(hit, locals.data() + hitOffset, 12);
    void* a = ret ? *reinterpret_cast<void**>(result) : nullptr;
    return a && ::Alive(a) ? a : nullptr;
}

void* Scripting::EditorCopy(void* original, const float loc[3], std::string& error)
{
    error.clear();
    if (!L) { error = "Lua isn't running"; return nullptr; }
    lua_getglobal(L, "__ed_copy");
    PushObject(L, original);
    lua_pushnumber(L, loc[0]);
    lua_pushnumber(L, loc[1]);
    lua_pushnumber(L, loc[2]);
    const int base = lua_gettop(L) - 5;
    if (!Protected(4, 1)) { error = "see the log"; lua_settop(L, base); return nullptr; }
    void* r = nullptr;
    if (auto* u = static_cast<LObj*>(luaL_testudata(L, -1, kObjMeta))) r = u->p;
    lua_settop(L, base);
    return r;
}

bool Scripting::EditorDelete(void* obj, std::string& error, int& belongings)
{
    error.clear();
    belongings = 0;
    if (!L) { error = "Lua isn't running"; return false; }
    lua_getglobal(L, "__ed_delete");
    PushObject(L, obj);
    const int base = lua_gettop(L) - 2;
    const bool ok = Protected(1, 2);
    if (ok) belongings = int(lua_tointeger(L, -1));
    lua_settop(L, base);
    if (!ok) error = "see the log";
    return ok;
}

std::string Scripting::MapName() { return Utf8(g_map.c_str(), g_map.size()); }

bool Engine::FromMap(void* obj) { return obj && ::Alive(obj) && Rd<void*>(obj, 0x10) != nullptr; }

bool Engine::GetBool(void* obj, const char* name) { return obj && ::Alive(obj) && ReadBool(obj, name); }
