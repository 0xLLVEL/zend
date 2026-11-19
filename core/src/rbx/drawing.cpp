#include "rbx/drawing.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lobject.h>
#include <lstate.h>
#include <lualib.h>

// Lua-facing Drawing API and the overlay renderer. The reference is
// sample/RBX/environment/libs/drawing: same property surface, document
// coordinates scaled by DPI, objects sorted by ZIndex then creation id.

namespace zend::rbx::drawing {
namespace {

enum class Kind : int {
    Line = 0,
    Text,
    Circle,
    Square,
    Triangle,
    Quad,
    Invalid
};

struct Object {
    std::uint64_t id = 0;
    std::size_t slot = 0;
    Kind kind = Kind::Invalid;
    bool visible = false;
    bool destroyed = false;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float transparency = 1.0f;
    float thickness = 1.0f;
    int zindex = 0;

    float fromX = 0.0f, fromY = 0.0f;
    float toX = 0.0f, toY = 0.0f;

    std::string text;
    float textSize = 13.0f;
    bool center = false;
    bool outline = false;
    float outR = 0.0f, outG = 0.0f, outB = 0.0f;
    float posX = 0.0f, posY = 0.0f;
    int font = 0;
    float textBoundsX = 0.0f, textBoundsY = 0.0f;

    float radius = 0.0f;
    int numSides = 24;
    bool filled = false;

    float sizeX = 0.0f, sizeY = 0.0f;

    float pAx = 0.0f, pAy = 0.0f;
    float pBx = 0.0f, pBy = 0.0f;
    float pCx = 0.0f, pCy = 0.0f;
    float pDx = 0.0f, pDy = 0.0f;
};

using ObjectRef = std::shared_ptr<Object>;

std::recursive_mutex g_mutex;
std::mutex g_font_mutex;
std::vector<ObjectRef> g_objects;
std::unordered_map<std::uint64_t, ObjectRef> g_lookup;
std::atomic<std::uint64_t> g_next_id{1};
std::atomic<float> g_scale{1.0f};
std::atomic<bool> g_font_ready{false};

constexpr char kMetaName[] = "ZendDrawing";

struct UdataPayload {
    std::uint64_t id;
};

int parse_kind(const char* name) {
    if (!name) return static_cast<int>(Kind::Invalid);
    if (!std::strcmp(name, "Line")) return static_cast<int>(Kind::Line);
    if (!std::strcmp(name, "Text")) return static_cast<int>(Kind::Text);
    if (!std::strcmp(name, "Circle")) return static_cast<int>(Kind::Circle);
    if (!std::strcmp(name, "Square")) return static_cast<int>(Kind::Square);
    if (!std::strcmp(name, "Triangle")) return static_cast<int>(Kind::Triangle);
    if (!std::strcmp(name, "Quad")) return static_cast<int>(Kind::Quad);
    return static_cast<int>(Kind::Invalid);
}

const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Line: return "Line";
    case Kind::Text: return "Text";
    case Kind::Circle: return "Circle";
    case Kind::Square: return "Square";
    case Kind::Triangle: return "Triangle";
    case Kind::Quad: return "Quad";
    default: return "Invalid";
    }
}

bool imgui_ready() {
    if (!g_font_ready.load(std::memory_order_acquire)) return false;
    return ImGui::GetCurrentContext() != nullptr;
}

ImFont* pick_font() {
    if (!imgui_ready()) return nullptr;
    return ImGui::GetIO().Fonts->Fonts[0];
}

void push_color3(lua_State* L, float r, float g, float b) {
    lua_getglobal(L, "Color3");
    if (!lua_istable(L, -1) && !lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushnumber(L, r); lua_setfield(L, -2, "R");
        lua_pushnumber(L, g); lua_setfield(L, -2, "G");
        lua_pushnumber(L, b); lua_setfield(L, -2, "B");
        return;
    }
    lua_getfield(L, -1, "new");
    lua_remove(L, -2);
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    if (lua_pcall(L, 3, 1, 0) != 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

void push_vector2(lua_State* L, float x, float y) {
    lua_getglobal(L, "Vector2");
    if (!lua_istable(L, -1) && !lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushnumber(L, x); lua_setfield(L, -2, "X");
        lua_pushnumber(L, y); lua_setfield(L, -2, "Y");
        return;
    }
    lua_getfield(L, -1, "new");
    lua_remove(L, -2);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

bool read_color3(lua_State* L, int idx, float& r, float& g, float& b) {
    if (lua_isnil(L, idx)) return false;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, "R");
    lua_getfield(L, idx, "G");
    lua_getfield(L, idx, "B");
    if (lua_isnumber(L, -3) && lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
        r = static_cast<float>(lua_tonumber(L, -3));
        g = static_cast<float>(lua_tonumber(L, -2));
        b = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 3);
        return true;
    }
    lua_pop(L, 3);
    return false;
}

bool read_vector2(lua_State* L, int idx, float& x, float& y) {
    if (lua_isnil(L, idx)) return false;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, "X");
    lua_getfield(L, idx, "Y");
    if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
        x = static_cast<float>(lua_tonumber(L, -2));
        y = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 2);
        return true;
    }
    lua_pop(L, 2);
    return false;
}

bool is_drawing_udata(lua_State* L, int idx) {
    if (!lua_isuserdata(L, idx)) return false;
    if (!lua_getmetatable(L, idx)) return false;
    luaL_getmetatable(L, kMetaName);
    const int equal = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return equal != 0;
}

ObjectRef resolve(lua_State* L, int idx) {
    if (!is_drawing_udata(L, idx)) return nullptr;
    auto* payload = static_cast<UdataPayload*>(lua_touserdata(L, idx));
    if (!payload) return nullptr;

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const auto it = g_lookup.find(payload->id);
    return it == g_lookup.end() ? nullptr : it->second;
}

void erase_object(const ObjectRef& target) {
    if (!target) return;

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const std::size_t slot = target->slot;
    if (slot < g_objects.size() && g_objects[slot] == target) {
        g_objects[slot] = g_objects.back();
        g_objects[slot]->slot = slot;
        g_objects.pop_back();
    }
    target->destroyed = true;
    target->visible = false;
    g_lookup.erase(target->id);
}

void recalc_text_bounds(Object* object) {
    if (!object || object->kind != Kind::Text) return;
    const float scale = g_scale.load(std::memory_order_relaxed);
    if (object->text.empty()) {
        object->textBoundsX = 0.0f;
        object->textBoundsY = object->textSize;
        return;
    }

    bool measured = false;
    float boundsX = 0.0f;
    float boundsY = 0.0f;
    {
        std::lock_guard<std::mutex> lock(g_font_mutex);
        ImFont* font = pick_font();
        if (font) {
            float size = object->textSize < 1.0f ? 1.0f : object->textSize;
            size *= scale > 0.0f ? scale : 1.0f;
            const ImVec2 value =
                font->CalcTextSizeA(size, FLT_MAX, 0.0f, object->text.c_str());
            boundsX = value.x;
            boundsY = value.y;
            measured = true;
        }
    }

    if (measured) {
        object->textBoundsX = boundsX;
        object->textBoundsY = boundsY;
        return;
    }
    object->textBoundsX =
        static_cast<float>(object->text.size()) * object->textSize * 0.55f;
    object->textBoundsY = object->textSize;
}

void ensure_metatable(lua_State* L);

int l_new(lua_State* L);
int l_destroy(lua_State* L);
int l_tostring(lua_State* L);
int l_index(lua_State* L);
int l_newindex(lua_State* L);
int l_clear_cache(lua_State* L);
int l_is_render_obj(lua_State* L);
int l_get_render_property(lua_State* L);
int l_set_render_property(lua_State* L);

void ensure_metatable(lua_State* L) {
    luaL_getmetatable(L, kMetaName);
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pop(L, 1);

    luaL_newmetatable(L, kMetaName);
    lua_pushcfunction(L, l_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_newindex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, l_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, "Drawing");
    lua_setfield(L, -2, "__type");
    lua_pushstring(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

int push_property(lua_State* L, const ObjectRef& ref, const char* key) {
    if (!std::strcmp(key, "Remove") || !std::strcmp(key, "Destroy")) {
        lua_pushcfunction(L, l_destroy, "Remove");
        return 1;
    }
    if (!ref) {
        if (!std::strcmp(key, "__OBJECT_EXISTS")) {
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Object* object = ref.get();

    if (!std::strcmp(key, "Visible")) { lua_pushboolean(L, object->visible); return 1; }
    if (!std::strcmp(key, "Transparency")) { lua_pushnumber(L, object->transparency); return 1; }
    if (!std::strcmp(key, "ZIndex")) { lua_pushinteger(L, object->zindex); return 1; }
    if (!std::strcmp(key, "Thickness")) { lua_pushnumber(L, object->thickness); return 1; }
    if (!std::strcmp(key, "Color")) { push_color3(L, object->r, object->g, object->b); return 1; }
    if (!std::strcmp(key, "__OBJECT_EXISTS")) { lua_pushboolean(L, !object->destroyed); return 1; }
    if (!std::strcmp(key, "Class") || !std::strcmp(key, "ClassName")) {
        lua_pushstring(L, kind_name(object->kind));
        return 1;
    }

    switch (object->kind) {
    case Kind::Line:
        if (!std::strcmp(key, "From")) { push_vector2(L, object->fromX, object->fromY); return 1; }
        if (!std::strcmp(key, "To")) { push_vector2(L, object->toX, object->toY); return 1; }
        break;
    case Kind::Text:
        if (!std::strcmp(key, "Text")) { lua_pushlstring(L, object->text.data(), object->text.size()); return 1; }
        if (!std::strcmp(key, "Size")) { lua_pushnumber(L, object->textSize); return 1; }
        if (!std::strcmp(key, "Center")) { lua_pushboolean(L, object->center); return 1; }
        if (!std::strcmp(key, "Outline")) { lua_pushboolean(L, object->outline); return 1; }
        if (!std::strcmp(key, "OutlineColor")) { push_color3(L, object->outR, object->outG, object->outB); return 1; }
        if (!std::strcmp(key, "Position")) { push_vector2(L, object->posX, object->posY); return 1; }
        if (!std::strcmp(key, "Font")) { lua_pushinteger(L, object->font); return 1; }
        if (!std::strcmp(key, "TextBounds")) {
            recalc_text_bounds(object);
            push_vector2(L, object->textBoundsX, object->textBoundsY);
            return 1;
        }
        break;
    case Kind::Circle:
        if (!std::strcmp(key, "Position")) { push_vector2(L, object->posX, object->posY); return 1; }
        if (!std::strcmp(key, "Radius")) { lua_pushnumber(L, object->radius); return 1; }
        if (!std::strcmp(key, "NumSides")) { lua_pushinteger(L, object->numSides); return 1; }
        if (!std::strcmp(key, "Filled")) { lua_pushboolean(L, object->filled); return 1; }
        break;
    case Kind::Square:
        if (!std::strcmp(key, "Position")) { push_vector2(L, object->posX, object->posY); return 1; }
        if (!std::strcmp(key, "Size")) { push_vector2(L, object->sizeX, object->sizeY); return 1; }
        if (!std::strcmp(key, "Filled")) { lua_pushboolean(L, object->filled); return 1; }
        break;
    case Kind::Triangle:
        if (!std::strcmp(key, "PointA")) { push_vector2(L, object->pAx, object->pAy); return 1; }
        if (!std::strcmp(key, "PointB")) { push_vector2(L, object->pBx, object->pBy); return 1; }
        if (!std::strcmp(key, "PointC")) { push_vector2(L, object->pCx, object->pCy); return 1; }
        if (!std::strcmp(key, "Filled")) { lua_pushboolean(L, object->filled); return 1; }
        break;
    case Kind::Quad:
        if (!std::strcmp(key, "PointA")) { push_vector2(L, object->pAx, object->pAy); return 1; }
        if (!std::strcmp(key, "PointB")) { push_vector2(L, object->pBx, object->pBy); return 1; }
        if (!std::strcmp(key, "PointC")) { push_vector2(L, object->pCx, object->pCy); return 1; }
        if (!std::strcmp(key, "PointD")) { push_vector2(L, object->pDx, object->pDy); return 1; }
        if (!std::strcmp(key, "Filled")) { lua_pushboolean(L, object->filled); return 1; }
        break;
    default:
        break;
    }

    lua_pushnil(L);
    return 1;
}

void assign_property(lua_State* L, const ObjectRef& ref, const char* key, int vidx) {
    if (!ref || ref->destroyed) return;
    vidx = lua_absindex(L, vidx);

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Object* object = ref.get();

    if (!std::strcmp(key, "Visible")) { object->visible = lua_toboolean(L, vidx) != 0; return; }
    if (!std::strcmp(key, "Transparency")) { object->transparency = static_cast<float>(luaL_checknumber(L, vidx)); return; }
    if (!std::strcmp(key, "ZIndex")) { object->zindex = luaL_checkinteger(L, vidx); return; }
    if (!std::strcmp(key, "Thickness")) { object->thickness = static_cast<float>(luaL_checknumber(L, vidx)); return; }
    if (!std::strcmp(key, "Color")) {
        float r = object->r, g = object->g, b = object->b;
        if (read_color3(L, vidx, r, g, b)) {
            object->r = r; object->g = g; object->b = b;
        }
        return;
    }

    switch (object->kind) {
    case Kind::Line:
        if (!std::strcmp(key, "From")) { read_vector2(L, vidx, object->fromX, object->fromY); return; }
        if (!std::strcmp(key, "To")) { read_vector2(L, vidx, object->toX, object->toY); return; }
        break;
    case Kind::Text:
        if (!std::strcmp(key, "Text")) {
            size_t length = 0;
            const char* text = luaL_checklstring(L, vidx, &length);
            object->text.assign(text ? text : "", length);
            return;
        }
        if (!std::strcmp(key, "Size")) { object->textSize = static_cast<float>(luaL_checknumber(L, vidx)); return; }
        if (!std::strcmp(key, "Center")) { object->center = lua_toboolean(L, vidx) != 0; return; }
        if (!std::strcmp(key, "Outline")) { object->outline = lua_toboolean(L, vidx) != 0; return; }
        if (!std::strcmp(key, "OutlineColor")) {
            float r = object->outR, g = object->outG, b = object->outB;
            if (read_color3(L, vidx, r, g, b)) {
                object->outR = r; object->outG = g; object->outB = b;
            }
            return;
        }
        if (!std::strcmp(key, "Position")) { read_vector2(L, vidx, object->posX, object->posY); return; }
        if (!std::strcmp(key, "Font")) {
            int font = luaL_checkinteger(L, vidx);
            if (font < 0) font = 0;
            if (font > 3) font = 3;
            object->font = font;
            return;
        }
        break;
    case Kind::Circle:
        if (!std::strcmp(key, "Position")) { read_vector2(L, vidx, object->posX, object->posY); return; }
        if (!std::strcmp(key, "Radius")) { object->radius = static_cast<float>(luaL_checknumber(L, vidx)); return; }
        if (!std::strcmp(key, "NumSides")) {
            int sides = luaL_checkinteger(L, vidx);
            if (sides < 3) sides = 3;
            if (sides > 128) sides = 128;
            object->numSides = sides;
            return;
        }
        if (!std::strcmp(key, "Filled")) { object->filled = lua_toboolean(L, vidx) != 0; return; }
        break;
    case Kind::Square:
        if (!std::strcmp(key, "Position")) { read_vector2(L, vidx, object->posX, object->posY); return; }
        if (!std::strcmp(key, "Size")) { read_vector2(L, vidx, object->sizeX, object->sizeY); return; }
        if (!std::strcmp(key, "Filled")) { object->filled = lua_toboolean(L, vidx) != 0; return; }
        break;
    case Kind::Triangle:
        if (!std::strcmp(key, "PointA")) { read_vector2(L, vidx, object->pAx, object->pAy); return; }
        if (!std::strcmp(key, "PointB")) { read_vector2(L, vidx, object->pBx, object->pBy); return; }
        if (!std::strcmp(key, "PointC")) { read_vector2(L, vidx, object->pCx, object->pCy); return; }
        if (!std::strcmp(key, "Filled")) { object->filled = lua_toboolean(L, vidx) != 0; return; }
        break;
    case Kind::Quad:
        if (!std::strcmp(key, "PointA")) { read_vector2(L, vidx, object->pAx, object->pAy); return; }
        if (!std::strcmp(key, "PointB")) { read_vector2(L, vidx, object->pBx, object->pBy); return; }
        if (!std::strcmp(key, "PointC")) { read_vector2(L, vidx, object->pCx, object->pCy); return; }
        if (!std::strcmp(key, "PointD")) { read_vector2(L, vidx, object->pDx, object->pDy); return; }
        if (!std::strcmp(key, "Filled")) { object->filled = lua_toboolean(L, vidx) != 0; return; }
        break;
    default:
        break;
    }
}

bool finite2(float x, float y) {
    return std::isfinite(x) && std::isfinite(y);
}

bool culled(float minX, float minY, float maxX, float maxY, const ImVec2& display) {
    constexpr float pad = 128.0f;
    if (maxX < -pad || maxY < -pad) return true;
    if (minX > display.x + pad || minY > display.y + pad) return true;
    return false;
}

ImU32 pack_color(float r, float g, float b, float a) {
    const auto channel = [](float value) -> int {
        int out = static_cast<int>(value * 255.0f + 0.5f);
        if (out < 0) out = 0;
        if (out > 255) out = 255;
        return out;
    };
    return IM_COL32(channel(r), channel(g), channel(b), channel(a));
}

int l_new(lua_State* L) {
    const char* class_name = luaL_checkstring(L, 1);
    const int kind = parse_kind(class_name);
    if (kind == static_cast<int>(Kind::Invalid)) {
        luaL_error(L, "invalid drawing class '%s'", class_name ? class_name : "");
        return 0;
    }

    auto object = std::make_shared<Object>();
    object->id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    object->kind = static_cast<Kind>(kind);
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        object->slot = g_objects.size();
        g_objects.push_back(object);
        g_lookup.emplace(object->id, object);
    }

    ensure_metatable(L);
    auto* payload = static_cast<UdataPayload*>(lua_newuserdata(L, sizeof(UdataPayload)));
    payload->id = object->id;
    luaL_getmetatable(L, kMetaName);
    lua_setmetatable(L, -2);
    return 1;
}

int l_destroy(lua_State* L) {
    if (ObjectRef object = resolve(L, 1)) {
        erase_object(object);
    }
    return 0;
}

int l_tostring(lua_State* L) {
    ObjectRef object = resolve(L, 1);
    if (!object) {
        lua_pushstring(L, "Drawing<destroyed>");
        return 1;
    }
    lua_pushfstring(L, "Drawing<%s>", kind_name(object->kind));
    return 1;
}

int l_index(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    return push_property(L, resolve(L, 1), key);
}

int l_newindex(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    assign_property(L, resolve(L, 1), key, 3);
    return 0;
}

int l_clear_cache(lua_State* L) {
    (void)L;
    reset();
    return 0;
}

int l_is_render_obj(lua_State* L) {
    lua_pushboolean(L, is_drawing_udata(L, 1) ? 1 : 0);
    return 1;
}

int l_get_render_property(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    return push_property(L, resolve(L, 1), key);
}

int l_set_render_property(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    assign_property(L, resolve(L, 1), key, 3);
    return 0;
}

} // namespace

void install(lua_State* L, RegisterFn register_global) {
    reset();
    ensure_metatable(L);

    lua_newtable(L);
    lua_pushcfunction(L, l_new, "new");
    lua_setfield(L, -2, "new");

    lua_newtable(L);
    lua_pushinteger(L, 0); lua_setfield(L, -2, "UI");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "System");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "Plex");
    lua_pushinteger(L, 3); lua_setfield(L, -2, "Monospace");
    lua_setfield(L, -2, "Fonts");

    lua_setglobal(L, "Drawing");

    register_global(L, "cleardrawcache", l_clear_cache);
    register_global(L, "clear_draw_cache", l_clear_cache);
    register_global(L, "isrenderobj", l_is_render_obj);
    register_global(L, "isrenderobject", l_is_render_obj);
    register_global(L, "is_render_obj", l_is_render_obj);
    register_global(L, "getrenderproperty", l_get_render_property);
    register_global(L, "get_render_property", l_get_render_property);
    register_global(L, "setrenderproperty", l_set_render_property);
    register_global(L, "set_render_property", l_set_render_property);
}

const lua_CFunction* own_functions(int* count) {
    static const lua_CFunction kFunctions[] = {
        l_new,       l_destroy,      l_tostring,
        l_index,     l_newindex,     l_clear_cache,
        l_is_render_obj, l_get_render_property, l_set_render_property,
    };
    if (count) {
        *count = static_cast<int>(sizeof(kFunctions) / sizeof(kFunctions[0]));
    }
    return kFunctions;
}

void reset() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (const auto& object : g_objects) {
        if (!object) continue;
        object->destroyed = true;
        object->visible = false;
    }
    g_objects.clear();
    g_lookup.clear();
}

void reset_renderer() {
    std::lock_guard<std::mutex> lock(g_font_mutex);
    g_font_ready.store(false, std::memory_order_release);
}

void render_all(ImDrawList* draw_list, float scale) {
    g_scale.store(scale > 0.0f ? scale : 1.0f, std::memory_order_relaxed);
    if (!draw_list) return;
    // Reached only from inside an ImGui frame, so the atlas is built.
    g_font_ready.store(true, std::memory_order_release);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;

    const float s = scale > 0.0f ? scale : 1.0f;

    // Snapshot under the lock, then draw without it. A fault in ImGui must not
    // leave the mutex held, or the next Lua property access would deadlock.
    static std::vector<Object> snapshot;
    static std::vector<Object*> ordered;
    snapshot.clear();
    ordered.clear();

    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_objects.empty()) return;
        snapshot.reserve(g_objects.size());
        for (const auto& object : g_objects) {
            if (object && object->visible && !object->destroyed) {
                snapshot.push_back(*object);
            }
        }
    }
    if (snapshot.empty()) return;

    ordered.reserve(snapshot.size());
    for (auto& object : snapshot) {
        ordered.push_back(&object);
    }

    std::sort(ordered.begin(), ordered.end(), [](const Object* a, const Object* b) {
        if (a->zindex != b->zindex) return a->zindex < b->zindex;
        return a->id < b->id;
    });

    for (Object* object : ordered) {
        float alpha = object->transparency;
        if (!std::isfinite(alpha)) continue;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        if (alpha <= 0.0f) continue;

        const ImU32 color = pack_color(object->r, object->g, object->b, alpha);
        const float thickness =
            (object->thickness < 1.0f || !std::isfinite(object->thickness))
                ? 1.0f
                : object->thickness;

        switch (object->kind) {
        case Kind::Line: {
            if (!finite2(object->fromX, object->fromY) ||
                !finite2(object->toX, object->toY)) break;
            const float fx = object->fromX * s, fy = object->fromY * s;
            const float tx = object->toX * s, ty = object->toY * s;
            if (culled((std::min)(fx, tx), (std::min)(fy, ty),
                       (std::max)(fx, tx), (std::max)(fy, ty), display)) break;
            draw_list->AddLine(ImVec2(fx, fy), ImVec2(tx, ty), color, thickness * s);
            break;
        }
        case Kind::Text: {
            if (object->text.empty() || !finite2(object->posX, object->posY)) break;
            ImFont* font = pick_font();
            if (!font) break;

            float render_size = object->textSize * s;
            if (!std::isfinite(render_size) || render_size < 1.0f) render_size = 1.0f;
            if (render_size > 128.0f) render_size = 128.0f;

            const ImVec2 measured = font->CalcTextSizeA(
                render_size, FLT_MAX, 0.0f, object->text.c_str());
            object->textBoundsX = measured.x;
            object->textBoundsY = measured.y;

            const float bx = object->posX * s, by = object->posY * s;
            float px = object->center ? (bx - measured.x * 0.5f) : bx;
            float py = by;
            if (culled(px, py, px + measured.x, py + measured.y, display)) break;

            px = floorf(px + 0.5f);
            py = floorf(py + 0.5f);
            if (object->outline) {
                const ImU32 outline_color =
                    pack_color(object->outR, object->outG, object->outB, alpha);
                const float offset = 1.0f * s;
                draw_list->AddText(font, render_size, ImVec2(px - offset, py), outline_color, object->text.c_str());
                draw_list->AddText(font, render_size, ImVec2(px + offset, py), outline_color, object->text.c_str());
                draw_list->AddText(font, render_size, ImVec2(px, py - offset), outline_color, object->text.c_str());
                draw_list->AddText(font, render_size, ImVec2(px, py + offset), outline_color, object->text.c_str());
            }
            draw_list->AddText(font, render_size, ImVec2(px, py), color, object->text.c_str());
            break;
        }
        case Kind::Circle: {
            if (!finite2(object->posX, object->posY) || !std::isfinite(object->radius)) break;
            if (object->radius <= 0.0f) break;
            const float cx = object->posX * s, cy = object->posY * s;
            const float radius = object->radius * s;
            if (culled(cx - radius, cy - radius, cx + radius, cy + radius, display)) break;

            int sides = object->numSides;
            if (sides < 3) sides = 3;
            if (sides > 128) sides = 128;
            if (object->filled) {
                draw_list->AddCircleFilled(ImVec2(cx, cy), radius, color, sides);
            } else {
                draw_list->AddCircle(ImVec2(cx, cy), radius, color, sides, thickness * s);
            }
            break;
        }
        case Kind::Square: {
            if (!finite2(object->posX, object->posY) || !finite2(object->sizeX, object->sizeY)) break;
            const ImVec2 a(object->posX * s, object->posY * s);
            const ImVec2 b((object->posX + object->sizeX) * s,
                           (object->posY + object->sizeY) * s);
            if (culled((std::min)(a.x, b.x), (std::min)(a.y, b.y),
                       (std::max)(a.x, b.x), (std::max)(a.y, b.y), display)) break;
            if (object->filled) {
                draw_list->AddRectFilled(a, b, color);
            } else {
                draw_list->AddRect(a, b, color, 0.0f, 0, thickness * s);
            }
            break;
        }
        case Kind::Triangle: {
            if (!finite2(object->pAx, object->pAy) || !finite2(object->pBx, object->pBy) ||
                !finite2(object->pCx, object->pCy)) break;
            const float minX = (std::min)(object->pAx, (std::min)(object->pBx, object->pCx)) * s;
            const float minY = (std::min)(object->pAy, (std::min)(object->pBy, object->pCy)) * s;
            const float maxX = (std::max)(object->pAx, (std::max)(object->pBx, object->pCx)) * s;
            const float maxY = (std::max)(object->pAy, (std::max)(object->pBy, object->pCy)) * s;
            if (culled(minX, minY, maxX, maxY, display)) break;

            const ImVec2 a(object->pAx * s, object->pAy * s);
            const ImVec2 b(object->pBx * s, object->pBy * s);
            const ImVec2 c(object->pCx * s, object->pCy * s);
            if (object->filled) {
                draw_list->AddTriangleFilled(a, b, c, color);
            } else {
                draw_list->AddTriangle(a, b, c, color, thickness * s);
            }
            break;
        }
        case Kind::Quad: {
            if (!finite2(object->pAx, object->pAy) || !finite2(object->pBx, object->pBy) ||
                !finite2(object->pCx, object->pCy) || !finite2(object->pDx, object->pDy)) break;
            const float minX = (std::min)((std::min)(object->pAx, object->pBx), (std::min)(object->pCx, object->pDx)) * s;
            const float minY = (std::min)((std::min)(object->pAy, object->pBy), (std::min)(object->pCy, object->pDy)) * s;
            const float maxX = (std::max)((std::max)(object->pAx, object->pBx), (std::max)(object->pCx, object->pDx)) * s;
            const float maxY = (std::max)((std::max)(object->pAy, object->pBy), (std::max)(object->pCy, object->pDy)) * s;
            if (culled(minX, minY, maxX, maxY, display)) break;

            const ImVec2 a(object->pAx * s, object->pAy * s);
            const ImVec2 b(object->pBx * s, object->pBy * s);
            const ImVec2 c(object->pCx * s, object->pCy * s);
            const ImVec2 d(object->pDx * s, object->pDy * s);
            if (object->filled) {
                draw_list->AddQuadFilled(a, b, c, d, color);
            } else {
                draw_list->AddQuad(a, b, c, d, color, thickness * s);
            }
            break;
        }
        default:
            break;
        }
    }
}

} // namespace zend::rbx::drawing
