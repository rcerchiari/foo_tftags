// foo_tftags - publishes long titleformat scripts as global tag fields.
//
// Any number of slots. Each slot has a tag name and a titleformat script. The result of the script is
// exposed to every component that evaluates title formatting as %tf_<name>%, for example
//   slot name "albumartist"  ->  %tf_albumartist%
// Scripts may be written over several lines: line breaks and leading indentation are removed
// before compiling, and // comments are dropped. Scripts may use the fields of other
// slots (%tf_artist% inside slot 3). Circular references (a slot that directly or indirectly
// uses its own field) are detected when Apply is pressed and refused with a message; the
// evaluator keeps a guard so a cycle can never loop.
//
// Field names are read by foobar2000 at startup, so a changed tag name offers to restart
// foobar2000 when Apply is pressed. Script changes apply immediately.
//
// NOTE: keep this file pure ASCII (use \u / \x escapes). MSVC reads BOM-less
// sources in the ANSI code page unless /utf-8 is set, which garbles literals.
//
// Build: VS2022, x64, C++17, link foobar2000_SDK, foobar2000_component_client, pfc.

#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/helpers/helpers.h>
#include <foobar2000/SDK/coreDarkMode.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

DECLARE_COMPONENT_VERSION("TF Tags", "0.1.2",
    "Publishes long titleformat scripts as global tag fields (%tf_<name>%).");
VALIDATE_COMPONENT_FILENAME("foo_tftags.dll");

namespace {

    constexpr size_t kLegacySlots = 20;  // older versions had 20 fixed slots
    constexpr const char* kFieldPrefix = "tf_";

    GUID makeGuid(std::uint32_t data1, std::uint8_t last) {
        return GUID{ data1, 0x4f2a, 0x4c3b, { 0x9d, 0x71, 0x2a, 0x5c, 0x80, 0x6e, 0x11, last } };
    }

    const GUID guid_tftags_preferences = {
        0x2d9b5e10, 0x7c34, 0x4a68, { 0x8e, 0x15, 0x61, 0xb3, 0x0f, 0x9a, 0x27, 0xc4 }
    };

    struct SlotConfig {
        GUID nameGuid;
        GUID scriptGuid;
        GUID tableGuid;
        cfg_string name;
        cfg_string script;
        cfg_string table;  // name of the replacement table (tables\<name>.json), "" = none
        SlotConfig(unsigned index, const char* defaultName, const char* defaultScript)
            : nameGuid(makeGuid(0x6f1c4a00u + index, 0x01)),
            scriptGuid(makeGuid(0x6f1c4b00u + index, 0x02)),
            tableGuid(makeGuid(0x6f1c4c10u + index, 0x05)),
            name(nameGuid, defaultName),
            script(scriptGuid, defaultScript),
            table(tableGuid, "") {
        }
    };

    // Only read to carry over the settings of older versions; slots are saved in g_slotsBlob now.
    SlotConfig g_legacySlots[kLegacySlots] = {
        SlotConfig(0, "01", ""),
        SlotConfig(1, "02", ""),
        SlotConfig(2, "03", ""),
        SlotConfig(3, "04", ""),
        SlotConfig(4, "05", ""),
        SlotConfig(5, "06", ""),
        SlotConfig(6, "07", ""),
        SlotConfig(7, "08", ""),
        SlotConfig(8, "09", ""),
        SlotConfig(9, "10", ""),
        SlotConfig(10, "11", ""),
        SlotConfig(11, "12", ""),
        SlotConfig(12, "13", ""),
        SlotConfig(13, "14", ""),
        SlotConfig(14, "15", ""),
        SlotConfig(15, "16", ""),
        SlotConfig(16, "17", ""),
        SlotConfig(17, "18", ""),
        SlotConfig(18, "19", ""),
        SlotConfig(19, "20", "")
    };

    // Language override ("" = follow the Windows display language, otherwise the code of a lang\<code>.lang file).
    cfg_string g_language(makeGuid(0x6f1c4c00u, 0x03), "");

    // ---- the slots: any number, saved as one text blob
    struct SlotData {
        std::uint32_t id = 0;  // stable: the registered field keeps pointing at its slot after edits
        std::string name;
        std::string script;    // LF line breaks
        std::string table;     // replacement table name, "" = none
    };

    cfg_string g_slotsBlob(makeGuid(0x6f1c4e00u, 0x07), "");

    std::string stripCr(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text)
            if (c != '\r') out += c;
        return out;
    }

    std::string escapeBlob(const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c != '\r') out += c;
        }
        return out;
    }

    std::string unescapeBlob(const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                if (text[i + 1] == 'n') { out += '\n'; ++i; continue; }
                if (text[i + 1] == '\\') { out += '\\'; ++i; continue; }
            }
            out += text[i];
        }
        return out;
    }

    std::string serializeSlots(const std::vector<SlotData>& slots) {
        std::string out = "v1\n";
        for (const SlotData& s : slots) {
            out += "id=" + std::to_string(s.id) + "\n";
            out += "name=" + escapeBlob(s.name) + "\n";
            out += "script=" + escapeBlob(s.script) + "\n";
            out += "table=" + escapeBlob(s.table) + "\n";
        }
        return out;
    }

    std::vector<SlotData> parseSlots(const std::string& blob) {
        std::vector<SlotData> slots;
        size_t pos = 0;
        while (pos < blob.size()) {
            size_t end = blob.find('\n', pos);
            if (end == std::string::npos) end = blob.size();
            const std::string line = blob.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string value = unescapeBlob(line.substr(eq + 1));
            if (key == "id") {
                SlotData slot;
                slot.id = (std::uint32_t)strtoul(value.c_str(), nullptr, 10);
                slots.push_back(slot);
            }
            else if (!slots.empty()) {
                if (key == "name") slots.back().name = value;
                else if (key == "script") slots.back().script = value;
                else if (key == "table") slots.back().table = value;
            }
        }
        // ids must be unique and non-zero
        std::unordered_set<std::uint32_t> seen;
        std::uint32_t maxId = 0;
        for (const SlotData& slot : slots) maxId = (std::max)(maxId, slot.id);
        for (SlotData& slot : slots) {
            if (slot.id == 0 || !seen.insert(slot.id).second) {
                slot.id = ++maxId;
                seen.insert(slot.id);
            }
        }
        return slots;
    }

    // The saved slots, or the used slots of an older version, or one empty slot.
    std::vector<SlotData> loadSlotsFromConfig() {
        std::vector<SlotData> slots;
        const char* blob = g_slotsBlob.get();
        if (blob && *blob) {
            slots = parseSlots(blob);
        }
        else {
            for (size_t i = 0; i < kLegacySlots; ++i) {
                const char* n = g_legacySlots[i].name.get();
                const char* v = g_legacySlots[i].script.get();
                const char* t = g_legacySlots[i].table.get();
                SlotData slot;
                slot.id = (std::uint32_t)(i + 1);
                slot.name = n ? n : "";
                slot.script = stripCr(v ? v : "");
                slot.table = t ? t : "";
                if (!slot.script.empty() || !slot.table.empty()) slots.push_back(slot);
            }
        }
        if (slots.empty()) {
            SlotData slot;
            slot.id = 1;
            slot.name = "01";
            slots.push_back(slot);
        }
        return slots;
    }

    // Thread-safe copy of the slots. Other threads (panels, search, background rendering)
    // evaluate slots while the main thread applies preferences, so they never read the
    // cfg_string objects directly.
    struct ScriptStore {
        std::mutex m;
        std::vector<SlotData> slots;
        bool loaded = false;
    };

    ScriptStore& scriptStore() {
        static ScriptStore* store = new ScriptStore;
        return *store;
    }

    std::atomic<unsigned> g_scriptVersion{ 0 };

    void loadStoreLocked(ScriptStore& st) {
        if (st.loaded) return;
        st.slots = loadSlotsFromConfig();
        st.loaded = true;
    }

    std::vector<SlotData> savedSlots() {
        ScriptStore& st = scriptStore();
        std::lock_guard<std::mutex> lock(st.m);
        loadStoreLocked(st);
        return st.slots;
    }

    bool getSlotById(std::uint32_t id, std::string& script, std::string& table) {
        ScriptStore& st = scriptStore();
        std::lock_guard<std::mutex> lock(st.m);
        loadStoreLocked(st);
        for (const SlotData& slot : st.slots) {
            if (slot.id == id) {
                script = slot.script;
                table = slot.table;
                return true;
            }
        }
        return false;
    }

    // Main thread only: saves the slots in the config and in the thread-safe copy.
    void setSlots(const std::vector<SlotData>& slots) {
        ScriptStore& st = scriptStore();
        std::lock_guard<std::mutex> lock(st.m);
        st.slots = slots;
        st.loaded = true;
        g_slotsBlob = serializeSlots(slots).c_str();
        ++g_scriptVersion;
    }

    // ---------------------------------------------------------------- helpers

    std::string cleanName(const std::string& raw) {
        std::string out;
        for (unsigned char c : raw) {
            if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out += static_cast<char>(c);
        }
        if (out.rfind(kFieldPrefix, 0) == 0) out.erase(0, 3);   // the tf_ prefix is added automatically
        return out;
    }

    // Field names of all slots, e.g. "tf_albumartist". Empty or duplicate names get a slot number.
    std::vector<std::string> fieldNames(const std::vector<std::string>& names) {
        std::vector<std::string> fields;
        fields.reserve(names.size());
        std::unordered_set<std::string> earlier;
        for (size_t i = 0; i < names.size(); ++i) {
            std::string base = cleanName(names[i]);
            if (base.empty()) base = "slot" + std::to_string(i + 1);
            fields.push_back(std::string(kFieldPrefix) + (earlier.count(base) ? base + "_" + std::to_string(i + 1) : base));
            earlier.insert(base);
        }
        return fields;
    }

    std::string fieldNameFor(size_t index, const std::vector<std::string>& names) {
        return fieldNames(names)[index];
    }

    std::vector<std::string> savedFieldNames() {
        std::vector<std::string> names;
        for (const SlotData& slot : savedSlots()) names.push_back(slot.name);
        return fieldNames(names);
    }

    // Make a multi-line script into the one-liner foobar2000 compiles: drop // comments (whole
    // line or trailing), strip leading/trailing whitespace of every line, join the lines.
    std::string flattenScript(const std::string& text) {
        std::string out;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            // Cut a // comment (whole-line or trailing). Text inside 'quotes' is literal and
            // "://" (as in a URL) does not start a comment.
            bool quoted = false;
            for (size_t i = 0; i + 1 < line.size(); ++i) {
                if (line[i] == '\'') quoted = !quoted;
                else if (!quoted && line[i] == '/' && line[i + 1] == '/' && (i == 0 || line[i - 1] != ':')) {
                    line.erase(i);
                    break;
                }
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t first = 0;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
            line.erase(0, first);
            if (!line.empty()) out += line;
            if (end == text.size()) break;
            pos = end + 1;
        }
        return out;
    }

    // Finds a reference cycle between slots (a slot using its own field, directly or through
    // other slots). Scans every script for %tf_<name>% references (text inside 'quotes' is
    // literal and skipped) and searches the reference graph iteratively (no recursion).
    // Returns e.g. "tf_a -> tf_b -> tf_a", or an empty string when there is no cycle.
    std::string findCycle(const std::vector<std::string>& names, const std::vector<std::string>& scripts) {
        const size_t count = names.size();
        const std::vector<std::string> field = fieldNames(names);
        std::unordered_map<std::string, size_t> indexOf;
        for (size_t i = 0; i < count; ++i) indexOf.emplace(field[i], i);
        std::vector<std::vector<size_t>> edges(count);

        for (size_t i = 0; i < count; ++i) {
            const std::string flat = flattenScript(scripts[i]);
            bool quoted = false;
            for (size_t p = 0; p < flat.size(); ++p) {
                const char c = flat[p];
                if (c == '\'') { quoted = !quoted; continue; }
                if (quoted || c != '%') continue;
                const size_t q = flat.find('%', p + 1);
                if (q == std::string::npos) break;
                std::string token = flat.substr(p + 1, q - p - 1);
                for (auto& ch : token) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                const auto hit = indexOf.find(token);
                if (hit != indexOf.end() && std::find(edges[i].begin(), edges[i].end(), hit->second) == edges[i].end())
                    edges[i].push_back(hit->second);
                p = q;
            }
        }

        for (size_t start = 0; start < count; ++start) {
            std::vector<size_t> parent(count, 0);
            std::vector<char> seen(count, 0);
            std::vector<size_t> queue;
            auto describe = [&](size_t last) {
                std::vector<size_t> path;
                for (size_t cur = last; cur != start; cur = parent[cur]) path.push_back(cur);
                path.push_back(start);
                std::reverse(path.begin(), path.end());
                std::string text;
                for (size_t n : path) text += field[n] + " -> ";
                return text + field[start];
                };
            for (size_t j : edges[start]) {
                if (j == start) return field[start] + " -> " + field[start];
                if (!seen[j]) { seen[j] = 1; parent[j] = start; queue.push_back(j); }
            }
            for (size_t head = 0; head < queue.size(); ++head) {
                const size_t u = queue[head];
                for (size_t v : edges[u]) {
                    if (v == start) return describe(u);
                    if (!seen[v]) { seen[v] = 1; parent[v] = u; queue.push_back(v); }
                }
            }
        }
        return {};
    }

    // ---------------------------------------------------------------- dynamic (ICY) metadata overlay

    using MetaValues = std::vector<std::string>;
    using MetaList = std::vector<std::pair<std::string, MetaValues>>;

    struct DynamicMetadata {
        std::mutex m;
        metadb_handle_ptr track;
        MetaList meta;
    };

    DynamicMetadata& dynamicMetadata() {
        static DynamicMetadata* d = new DynamicMetadata;
        return *d;
    }

    class dynamic_metadata_callback : public play_callback_static {
    public:
        unsigned get_flags() override {
            return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_dynamic_info_track;
        }

        void on_playback_new_track(metadb_handle_ptr track) override {
            DynamicMetadata& d = dynamicMetadata();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.track = track;
                d.meta.clear();
            }
        }

        void on_playback_stop(play_control::t_stop_reason) override {
            DynamicMetadata& d = dynamicMetadata();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.track.release();
                d.meta.clear();
            }
        }

        void on_playback_dynamic_info_track(const file_info& info) override {
            MetaList meta;
            const t_size total = info.meta_get_count();
            for (t_size i = 0; i < total; ++i) {
                const t_size values = info.meta_enum_value_count(i);
                if (values == 0) continue;
                std::string name = info.meta_enum_name(i);
                for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                // Keep every value, like normal metadata fields.
                MetaValues list;
                for (t_size v = 0; v < values; ++v) list.emplace_back(info.meta_enum_value(i, v));
                meta.emplace_back(std::move(name), std::move(list));
            }
            DynamicMetadata& d = dynamicMetadata();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.meta = std::move(meta);
            }
        }

        void on_playback_starting(play_control::t_track_command, bool) override {}
        void on_playback_seek(double) override {}
        void on_playback_pause(bool) override {}
        void on_playback_edited(metadb_handle_ptr) override {}
        void on_playback_dynamic_info(const file_info&) override {}
        void on_playback_time(double) override {}
        void on_volume_change(float) override {}
    };

    play_callback_static_factory_t<dynamic_metadata_callback> g_dynamic_metadata_callback;

    // Live stream metadata (ICY etc.) of the playing track, if it has any. Only %field% and
    // $meta(field[, n]), $meta_sep() and $meta_num() are overlaid; %isplaying% and everything
    // else is left to foobar2000.
    bool dynamicOverlayFor(metadb_handle* handle, MetaList& out) {
        DynamicMetadata& d = dynamicMetadata();
        std::lock_guard<std::mutex> lock(d.m);
        if (!d.track.is_valid() || d.track.get_ptr() != handle || d.meta.empty()) return false;
        out = d.meta;
        return true;
    }

    void applyTable(const std::string& name, std::string& value);   // defined with the tables below
    std::vector<std::string> tableFields(const std::string& name);   // lower-case multi-value field names

    // Hook used while a slot's script is formatted. It answers %field%, $meta(field[, n]),
    // $meta_sep() and $meta_num() from (1) the live stream metadata of the playing track and
    // (2) when the slot has a replacement table, the real values of the table's multi-value
    // fields (ARTIST, GENRE, ... by default), replacing every value on its own. Everything else is
    // left to foobar2000.
    class slot_hook : public titleformat_hook {
    public:
        slot_hook(MetaList overlay, metadb_info_container::ptr info, const std::string& table) :
            m_items(std::move(overlay)), m_info(std::move(info)), m_table(table) {
            if (!m_table.empty())
                for (const std::string& field : tableFields(m_table)) m_fields.insert(field);
        }

        // %field%: all values joined with ", " like foobar2000 does for multi-value fields.
        bool process_field(titleformat_text_out* out, const char* name, t_size length,
            bool& found) override {
            MetaValues values;
            if (!valuesFor(std::string(name, length), values)) return false;
            write(out, join(values, ", ", ", "));
            found = true;
            return true;
        }

        // $meta(field), $meta(field, n), $meta_sep(field, sep [, lastsep]) and $meta_num(field).
        bool process_function(titleformat_text_out* out, const char* name, t_size length,
            titleformat_hook_function_params* params, bool& found) override {
            const bool isMeta = length == 4 && _strnicmp(name, "meta", 4) == 0;
            const bool isSep = length == 8 && _strnicmp(name, "meta_sep", 8) == 0;
            const bool isNum = length == 8 && _strnicmp(name, "meta_num", 8) == 0;
            if ((!isMeta && !isSep && !isNum) || params->get_param_count() < 1) return false;

            const char* field = nullptr;
            t_size fieldLength = 0;
            params->get_param(0, field, fieldLength);
            MetaValues values;
            if (!valuesFor(std::string(field, fieldLength), values)) return false;

            if (isNum) {
                write(out, std::to_string(values.size()));
                found = true;
                return true;
            }
            if (isSep) {
                if (params->get_param_count() < 2) return false;
                const char* sep = nullptr;
                t_size sepLength = 0;
                params->get_param(1, sep, sepLength);
                std::string last(sep, sepLength);
                if (params->get_param_count() >= 3) {
                    const char* lastSep = nullptr;
                    t_size lastLength = 0;
                    params->get_param(2, lastSep, lastLength);
                    last.assign(lastSep, lastLength);
                }
                write(out, join(values, std::string(sep, sepLength), last));
                found = true;
                return true;
            }
            const t_size index = params->get_param_count() >= 2 ? params->get_param_uint(1) : 0;
            if (index >= values.size()) {
                found = false;  // the field exists, but not that many values
                return true;
            }
            write(out, values[index]);
            found = true;
            return true;
        }

    private:
        static void write(titleformat_text_out* out, const std::string& text) {
            out->write(titleformat_inputtypes::meta, text.c_str(), text.length());
        }

        static std::string join(const MetaValues& values, const std::string& sep, const std::string& lastSep) {
            std::string text;
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) text += (i + 1 == values.size()) ? lastSep : sep;
                text += values[i];
            }
            return text;
        }

        // The values of a field, or false to leave the field to foobar2000.
        bool valuesFor(const std::string& name, MetaValues& out) const {
            std::string key = name;
            for (auto& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            bool have = false;
            for (const auto& item : m_items)
                if (item.first == key) { out = item.second; have = true; break; }
            const bool listed = !m_table.empty() && m_fields.count(key) > 0;
            if (!have && listed && m_info.is_valid()) have = fileValues(key, out);
            if (!have || out.empty()) return false;
            if (listed)
                for (std::string& value : out) applyTable(m_table, value);
            return true;
        }

        bool fileValues(const std::string& key, MetaValues& out) const {
            const file_info& info = m_info->info();
            t_size index = info.meta_find(key.c_str());
            if (index == pfc_infinite && key == "album artist") index = info.meta_find("albumartist");
            if (index == pfc_infinite) return false;
            const t_size count = info.meta_enum_value_count(index);
            for (t_size v = 0; v < count; ++v) out.emplace_back(info.meta_enum_value(index, v));
            return true;
        }

        MetaList m_items;
        metadb_info_container::ptr m_info;
        std::string m_table;
        std::unordered_set<std::string> m_fields;
    };

    // ---------------------------------------------------------------- evaluation

    // Cache compiled titleformat objects only. The script version is compared on every
    // request so preference edits take effect immediately; evaluated values are never cached.
    struct ScriptCache {
        std::mutex m;
        unsigned version = 0;
        titleformat_object::ptr obj;
        std::string table;
        bool init = false;
    };

    struct CompiledSlot {
        titleformat_object::ptr obj;
        std::string table;
    };

    struct CacheMap {
        std::mutex m;
        std::unordered_map<std::uint32_t, ScriptCache*> map;
    };

    CacheMap& cacheMap() {
        static CacheMap* caches = new CacheMap;
        return *caches;
    }

    ScriptCache& scriptCache(std::uint32_t id) {
        CacheMap& cm = cacheMap();
        std::lock_guard<std::mutex> lock(cm.m);
        ScriptCache*& cache = cm.map[id];
        if (!cache) cache = new ScriptCache;
        return *cache;
    }

    // A script that does not compile gives no object, so its field is empty.
    CompiledSlot compileScript(std::uint32_t id) {
        ScriptCache& c = scriptCache(id);
        const unsigned version = g_scriptVersion.load();
        {
            std::lock_guard<std::mutex> lock(c.m);
            if (c.init && c.version == version) return CompiledSlot{ c.obj, c.table };
        }
        std::string script, table;
        if (!getSlotById(id, script, table)) return CompiledSlot{};  // the slot was removed
        const std::string flat = flattenScript(script);
        titleformat_object::ptr obj;
        if (!flat.empty() && !titleformat_compiler::get()->compile(obj, flat.c_str())) obj.release();
        std::lock_guard<std::mutex> lock(c.m);
        c.obj = obj;
        c.table = table;
        c.version = version;
        c.init = true;
        return CompiledSlot{ obj, table };
    }

    class script_cache_cleanup : public initquit {
    public:
        void on_quit() override {
            {
                DynamicMetadata& d = dynamicMetadata();
                std::lock_guard<std::mutex> lock(d.m);
                d.track.release();
                d.meta.clear();
            }
            CacheMap& cm = cacheMap();
            std::lock_guard<std::mutex> mapLock(cm.m);
            for (auto& entry : cm.map) {
                ScriptCache& c = *entry.second;
                std::lock_guard<std::mutex> lock(c.m);
                c.obj.release();
                c.init = false;
            }
        }
    };
    initquit_factory_t<script_cache_cleanup> g_script_cache_cleanup;

    // ---------------------------------------------------------------- replacement tables
    // A table is a JSON file in <profile>\foo_tftags\tables\<name>.json that maps names to the
    // value they should become: { "R.E.M.": ["REM", "R.E.M"], "T. Rex": "T REX" }. The key is
    // the wanted result, the value is one name or a list of names that are replaced by it
    // (whole value, case-insensitive). A slot can run its result through one table.
    std::string foldCase(const std::string& text) {
        if (text.empty()) return text;
        pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
        const std::wstring w(wide.get_ptr());
        std::wstring lower(w.size(), L'\0');
        const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), (int)w.size(),
            &lower[0], (int)lower.size(), nullptr, nullptr, 0);
        if (n <= 0) lower = w;
        else lower.resize((size_t)n);
        pfc::stringcvt::string_utf8_from_wide utf8(lower.c_str());
        return std::string(utf8.get_ptr());
    }

    void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // Minimal JSON reader for the flat shape above (strings, arrays of strings, one object).
    class JsonReader {
    public:
        explicit JsonReader(const std::string& text) : m_s(text) {
            if (m_s.compare(0, 3, "\xEF\xBB\xBF") == 0) m_pos = 3;
        }

        bool eat(char c) {
            skip();
            if (m_pos < m_s.size() && m_s[m_pos] == c) { ++m_pos; return true; }
            return false;
        }

        bool peek(char c) {
            skip();
            return m_pos < m_s.size() && m_s[m_pos] == c;
        }

        bool atEnd() {
            skip();
            return m_pos >= m_s.size();
        }

        bool readString(std::string& out) {
            out.clear();
            if (!eat('"')) return false;
            while (m_pos < m_s.size()) {
                const char c = m_s[m_pos++];
                if (c == '"') return true;
                if (c != '\\') { out += c; continue; }
                if (m_pos >= m_s.size()) return false;
                const char e = m_s[m_pos++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && m_pos + 1 < m_s.size() && m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u') {
                        const size_t save = m_pos;
                        m_pos += 2;
                        unsigned low = 0;
                        if (hex4(low) && low >= 0xDC00 && low < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        else m_pos = save;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            }
            return false;
        }

    private:
        void skip() {
            while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\r' || m_s[m_pos] == '\n')) ++m_pos;
        }

        bool hex4(unsigned& cp) {
            if (m_pos + 4 > m_s.size()) return false;
            cp = 0;
            for (int k = 0; k < 4; ++k) {
                const char h = m_s[m_pos++];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                else return false;
            }
            return true;
        }

        const std::string& m_s;
        size_t m_pos = 0;
    };

    // Builds folded-name -> result. Every wanted result also maps to itself, which fixes its case.
    bool parseTableJson(const std::string& json, std::unordered_map<std::string, std::string>& out,
        std::vector<std::string>& separators, std::vector<std::string>& fields) {
        JsonReader r(json);
        if (!r.eat('{')) return false;
        std::vector<std::string> targets;
        bool sawSplit = false;
        bool sawFields = false;
        if (!r.eat('}')) {
            for (;;) {
                std::string target;
                if (!r.readString(target) || !r.eat(':')) return false;
                std::vector<std::string> sources;
                if (r.peek('"')) {
                    std::string one;
                    if (!r.readString(one)) return false;
                    sources.push_back(one);
                }
                else if (r.eat('[')) {
                    if (!r.eat(']')) {
                        for (;;) {
                            std::string one;
                            if (!r.readString(one)) return false;
                            sources.push_back(one);
                            if (r.eat(',')) continue;
                            if (r.eat(']')) break;
                            return false;
                        }
                    }
                }
                else return false;
                if (target == "$split") {
                    // Not a name: the separators that split a multi-value result into names.
                    sawSplit = true;
                    for (const std::string& source : sources)
                        if (!source.empty()) separators.push_back(source);
                }
                else if (target == "$fields") {
                    // Not a name: the multi-value tag fields whose values are replaced one by one.
                    sawFields = true;
                    for (const std::string& source : sources)
                        if (!source.empty()) fields.push_back(foldCase(source));
                }
                else {
                    for (const std::string& source : sources) out[foldCase(source)] = target;
                    targets.push_back(target);
                }
                if (r.eat(',')) continue;
                if (r.eat('}')) break;
                return false;
            }
        }
        if (!r.atEnd()) return false;
        for (const std::string& target : targets) out.emplace(foldCase(target), target);
        if (!sawSplit) separators.push_back(";");  // default: "Korn;Madonna" and "Korn; Madonna"
        if (!sawFields) {
            for (const char* field : { "artist", "album artist", "producer", "composer", "performer", "genre", "style", "country" })
                fields.push_back(field);
        }
        return true;
    }

    bool validTableName(const std::string& name) {
        return !name.empty() && name[0] != '.' && name.find_first_of("\\/:*?\"<>|") == std::string::npos;
    }

    std::wstring profileDirectory() {
        pfc::string8 native;
        try {
            filesystem::g_get_native_path(core_api::get_profile_path(), native);
        }
        catch (...) {
            return std::wstring();
        }
        pfc::stringcvt::string_wide_from_utf8 wide(native.c_str());
        std::wstring dir(wide.get_ptr());
        if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
        return dir;
    }

    // <profile>\foo_tftags\tables\ (with a trailing backslash), or empty when there is no profile.
    std::wstring tablesDirectory(bool create) {
        std::wstring dir = profileDirectory();
        if (dir.empty()) return dir;
        dir += L"foo_tftags\\";
        if (create) CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"tables\\";
        if (create) CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    std::vector<std::string> listTables() {
        std::vector<std::string> names;
        const std::wstring dir = tablesDirectory(false);
        if (dir.empty()) return names;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return names;
        do {
            const std::wstring file = fd.cFileName;
            if (file.size() <= 5 || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            pfc::stringcvt::string_utf8_from_wide utf8(file.substr(0, file.size() - 5).c_str());
            const std::string name(utf8.get_ptr());
            if (validTableName(name)) names.push_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end());
        return names;
    }

    struct TableData {
        std::unordered_map<std::string, std::string> map;
        std::vector<std::string> separators;  // from the "$split" entry
        std::vector<std::string> fields;      // from the "$fields" entry (lower case)
        FILETIME stamp{};
        DWORD lastCheck = 0;
        bool loaded = false;
        int state = 1;  // 0 = ok, 1 = file missing, 2 = not valid JSON of the expected shape
    };

    struct TableRegistry {
        std::mutex m;
        std::unordered_map<std::string, TableData> tables;
    };

    TableRegistry& tableRegistry() {
        static TableRegistry* reg = new TableRegistry;
        return *reg;
    }

    bool readTableFile(const std::wstring& path, std::string& out) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }

    // Caller holds the registry lock. The file is checked at most every 2 seconds (always when
    // `force`), so edits to a table are picked up without a restart.
    TableData& refreshTable(TableRegistry& reg, const std::string& name, bool force) {
        TableData& t = reg.tables[name];
        const DWORD now = GetTickCount();
        if (t.loaded && !force && now - t.lastCheck < 2000) return t;
        t.lastCheck = now;

        const std::wstring dir = tablesDirectory(false);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        std::wstring path;
        if (!dir.empty() && validTableName(name)) {
            pfc::stringcvt::string_wide_from_utf8 wide(name.c_str());
            path = dir + wide.get_ptr() + L".json";
        }
        if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            t.map.clear();
            t.separators.clear();
            t.fields.clear();
            t.state = 1;
            t.stamp = FILETIME{};
            t.loaded = true;
            return t;
        }
        if (t.loaded && CompareFileTime(&fad.ftLastWriteTime, &t.stamp) == 0) return t;

        std::string data;
        std::unordered_map<std::string, std::string> map;
        std::vector<std::string> separators, fields;
        if (readTableFile(path, data) && parseTableJson(data, map, separators, fields)) {
            t.map.swap(map);
            t.separators.swap(separators);
            t.fields.swap(fields);
            t.state = 0;
        }
        else {
            t.map.clear();
            t.separators.clear();
            t.fields.clear();
            t.state = 2;
        }
        t.stamp = fad.ftLastWriteTime;
        t.loaded = true;
        return t;
    }

    // 0 = table usable, 1 = file missing, 2 = file not valid. Re-reads the file if it changed.
    int tableState(const std::string& name) {
        TableRegistry& reg = tableRegistry();
        std::lock_guard<std::mutex> lock(reg.m);
        return refreshTable(reg, name, true).state;
    }

    std::vector<std::string> tableFields(const std::string& name) {
        if (name.empty()) return {};
        TableRegistry& reg = tableRegistry();
        std::lock_guard<std::mutex> lock(reg.m);
        const TableData& t = refreshTable(reg, name, false);
        return t.state == 0 ? t.fields : std::vector<std::string>();
    }

    bool lookupFolded(const TableData& t, const std::string& text, std::string& replacement) {
        const auto it = t.map.find(foldCase(text));
        if (it == t.map.end()) return false;
        replacement = it->second;
        return true;
    }

    // Replaces `value` using the table (whole value, case-insensitive). When the table has a
    // "$split" entry (default: ";"), a value that is not found as a whole is split at those
    // separators (e.g. "A;B", "A; B" or "A, B") and every name is replaced on its own. A run of several parts is tried
    // first (longest first), so names that contain a separator ("Earth, Wind & Fire") still match.
    // The original separators and surrounding spaces are kept.
    void applyTable(const std::string& name, std::string& value) {
        if (name.empty() || value.empty()) return;
        TableRegistry& reg = tableRegistry();
        std::lock_guard<std::mutex> lock(reg.m);
        TableData& t = refreshTable(reg, name, false);
        if (t.state != 0) return;

        std::string hit;
        if (lookupFolded(t, value, hit)) {
            value = hit;
            return;
        }
        if (t.separators.empty()) return;

        std::vector<std::pair<size_t, size_t>> parts;  // [begin, end) of every name
        size_t begin = 0, pos = 0;
        while (pos < value.size()) {
            size_t sepLen = 0;
            for (const std::string& sep : t.separators)
                if (sep.size() > sepLen && value.compare(pos, sep.size(), sep) == 0) sepLen = sep.size();
            if (sepLen > 0) {
                parts.emplace_back(begin, pos);
                pos += sepLen;
                begin = pos;
            }
            else {
                ++pos;
            }
        }
        parts.emplace_back(begin, value.size());
        if (parts.size() < 2) return;

        const size_t kMaxRun = 6;
        std::string out;
        bool changed = false;
        size_t i = 0;
        while (i < parts.size()) {
            size_t next = i + 1;
            bool matched = false;
            for (size_t j = (std::min)(parts.size(), i + kMaxRun); j > i && !matched; --j) {
                const size_t origBegin = parts[i].first, origEnd = parts[j - 1].second;
                size_t b = origBegin, e = origEnd;
                while (b < e && (value[b] == ' ' || value[b] == '\t')) ++b;
                while (e > b && (value[e - 1] == ' ' || value[e - 1] == '\t')) --e;
                std::string replacement;
                if (b < e && lookupFolded(t, value.substr(b, e - b), replacement)) {
                    out.append(value, origBegin, b - origBegin);
                    out += replacement;
                    out.append(value, e, origEnd - e);
                    matched = true;
                    changed = true;
                    next = j;
                }
            }
            if (!matched) out.append(value, parts[i].first, parts[i].second - parts[i].first);
            if (next < parts.size()) out.append(value, parts[next - 1].second, parts[next].first - parts[next - 1].second);
            i = next;
        }
        if (changed) value = out;
    }

    // Formats `h` with a slot script: live stream metadata for the playing track and, when the
    // slot has a replacement table, value-by-value replacement of the table's multi-value fields.
    void formatSlot(metadb_handle* h, const titleformat_object::ptr& script, const std::string& table, pfc::string8& out) {
        MetaList overlay;
        const bool live = dynamicOverlayFor(h, overlay);
        metadb_info_container::ptr info;
        if (!table.empty()) h->get_info_ref(info);  // not available until the file has been scanned
        if (live || info.is_valid()) {
            slot_hook hook(std::move(overlay), info, table);
            h->format_title(&hook, out, script, nullptr);
        }
        else {
            h->format_title(nullptr, out, script, nullptr);
        }
    }

    // Evaluates one slot for one track. A slot that is already being evaluated on this thread
    // (a reference cycle) returns an empty string.
    std::string evalSlot(metadb_handle* h, std::uint32_t id) {
        thread_local std::vector<std::uint32_t> inProgress;
        if (std::find(inProgress.begin(), inProgress.end(), id) != inProgress.end()) return {};

        std::string value;
        const CompiledSlot compiled = compileScript(id);
        const titleformat_object::ptr& script = compiled.obj;
        if (script.is_valid()) {
            struct Guard { std::vector<std::uint32_t>& list; ~Guard() { list.pop_back(); } } guard{ inProgress };
            inProgress.push_back(id);
            pfc::string8 result;
            formatSlot(h, script, compiled.table, result);
            value = result.c_str();
            applyTable(compiled.table, value);
        }
        return value;
    }

    // The fields foobar2000 knows, fixed when it first asks (at startup). A slot is found by its
    // id, so edits keep working under the registered name; added, renamed or removed fields need
    // a restart.
    struct RegisteredFields {
        std::vector<std::uint32_t> ids;
        std::vector<std::string> names;
    };

    const RegisteredFields& registeredFields() {
        static const RegisteredFields registered = [] {
            RegisteredFields r;
            std::vector<std::string> names;
            for (const SlotData& slot : savedSlots()) {
                r.ids.push_back(slot.id);
                names.push_back(slot.name);
            }
            r.names = fieldNames(names);
            return r;
            }();
        return registered;
    }

    class tf_fields : public metadb_display_field_provider {
    public:
        t_uint32 get_field_count() override { return (t_uint32)registeredFields().ids.size(); }

        void get_field_name(t_uint32 index, pfc::string_base& out) override {
            const RegisteredFields& r = registeredFields();
            if (index >= r.names.size()) { out.reset(); return; }
            out.set_string(r.names[index].c_str());
        }

        bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override {
            const RegisteredFields& r = registeredFields();
            if (index >= r.ids.size()) return false;
            const std::string value = evalSlot(handle, r.ids[index]);
            if (value.empty()) return false;
            out->write(titleformat_inputtypes::meta, value.c_str(), value.length());
            return true;
        }
    };
    service_factory_single_t<tf_fields> g_tf_fields;

    // ---------------------------------------------------------------- translations
    // Texts live in plain UTF-8 files (lang\<code>.lang, key=text per line, \n for a line break,
    // # starts a comment) next to the component. Anything missing falls back to English.
    struct DefaultText { const char* key; const char* text; };
    const DefaultText kDefaultTexts[] = {
        { "name_label", "Tag name (the field is always prefixed with tf_):" },
        { "script_label", "Titleformat script:" },
        { "use_in_tf", "Use in title formatting:" },
        { "note_1", "Scripts may use other slots (%tf_artist%); circular references are refused when you press Apply." },
        { "note_3", "Adding, renaming or removing a tag asks to restart foobar2000 when you press Apply; script changes apply at once." },
        { "cycle_apply", "Circular reference between slots: {0}\n\nNothing was applied. Remove the reference and press Apply again." },
        { "cycle_test", "Circular reference between slots: {0}" },
        { "broken_scripts", "These scripts do not compile, so their fields will be empty:{0}\n\nApply anyway?" },
        { "restart_prompt", "The list of tag names changed. foobar2000 reads field names at startup, so added, renamed or removed fields only take effect after a restart.\n\nRestart foobar2000 now?" },
        { "test_no_track", "Select a track in the playlist (or start playback) and try again." },
        { "test_empty_script", "(empty script or the script does not compile)" },
        { "test_empty_result", "(empty result)" },
        { "preview_label", "Preview:" },
        { "editor_title", "Titleformat script" },
        { "editor_discard", "Discard your changes to the script?" },
        { "btn_cancel", "Cancel" },
        { "import_button", "Import..." },
        { "export_button", "Export..." },
        { "import_title", "Import settings" },
        { "export_title", "Export settings" },
        { "filter_settings", "TF Tags settings (*.tftags)" },
        { "filter_all", "All files (*.*)" },
        { "import_confirm", "Add the slots in this file to the page? A slot with the same tag name is replaced.\n\nThey are applied when you press Apply." },
        { "import_empty", "The file has no slots with a script." },
        { "add_slot", "Add" },
        { "remove_slot", "Remove" },
        { "remove_confirm", "Remove the slot {0}?\n\nThe field stops working after you apply the change and restart foobar2000." },
        { "import_invalid", "This is not a TF Tags settings file." },
        { "import_failed", "The settings file could not be read." },
        { "export_failed", "The settings file could not be written." },
        { "table_label", "Replacement table:" },
        { "table_none", "(none)" },
        { "table_folder", "Open folder" },
        { "table_error", "The replacement table \"{0}\" is missing or is not valid." },
        { "lang_label", "Language:" },
        { "lang_auto", "Automatic (Windows display language)" },
        { "btn_yes", "Yes" },
        { "btn_no", "No" },
        { "btn_ok", "OK" },
    };

    struct LanguageTable {
        std::unordered_map<std::string, std::string> table;
        bool loaded = false;
    };

    LanguageTable& languageTable() {
        static LanguageTable* t = new LanguageTable;
        return *t;
    }

    void parseLanguageFile(const std::string& data, std::unordered_map<std::string, std::string>& out) {
        size_t pos = data.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        while (pos < data.size()) {
            size_t end = data.find('\n', pos);
            if (end == std::string::npos) end = data.size();
            std::string line = data.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            const std::string raw = line.substr(eq + 1);
            std::string value;
            for (size_t k = 0; k < raw.size(); ++k) {
                if (raw[k] == '\\' && k + 1 < raw.size()) {
                    const char n = raw[k + 1];
                    if (n == 'n') { value += '\n'; ++k; continue; }
                    if (n == '\\') { value += '\\'; ++k; continue; }
                }
                value += raw[k];
            }
            out[key] = value;
        }
    }

    bool readFileText(const std::wstring& path, std::string& out) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }

    // The "lang" folder next to the component DLL (with a trailing backslash), or empty.
    std::wstring languageDirectory() {
        wchar_t modulePath[MAX_PATH * 2] = {};
        const DWORD len = GetModuleFileNameW(core_api::get_my_instance(), modulePath, (DWORD)(sizeof(modulePath) / sizeof(wchar_t)));
        if (len == 0) return std::wstring();
        std::wstring dir(modulePath, len);
        const size_t slash = dir.find_last_of(L"\\/");
        dir.resize(slash == std::wstring::npos ? 0 : slash + 1);
        return dir + L"lang\\";
    }

    struct LanguageEntry {
        std::string code;
        std::string name;
    };

    // All lang\*.lang files; the display name comes from their language_name key.
    std::vector<LanguageEntry> listLanguages() {
        std::vector<LanguageEntry> list;
        const std::wstring dir = languageDirectory();
        if (dir.empty()) return list;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"*.lang").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return list;
        do {
            const std::wstring file = fd.cFileName;
            if (file.size() <= 5) continue;
            std::string code;
            bool ascii = true;
            for (size_t k = 0; k + 5 < file.size(); ++k) {
                if (file[k] > 127) { ascii = false; break; }
                code += static_cast<char>(file[k]);
            }
            if (!ascii || code.empty()) continue;
            LanguageEntry entry{ code, code };
            std::string data;
            std::unordered_map<std::string, std::string> table;
            if (readFileText(dir + file, data)) {
                parseLanguageFile(data, table);
                const auto it = table.find("language_name");
                if (it != table.end() && !it->second.empty()) entry.name = it->second;
            }
            list.push_back(entry);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(list.begin(), list.end(), [](const LanguageEntry& x, const LanguageEntry& y) { return x.code < y.code; });
        return list;
    }

    // Loads lang\<code>.lang: the chosen override, otherwise the Windows display language
    // (e.g. zh-CN.lang, then zh.lang).
    void loadLanguage(std::unordered_map<std::string, std::string>& out) {
        const std::wstring dir = languageDirectory();
        if (dir.empty()) return;

        std::vector<std::string> candidates;
        const char* forced = g_language.get();
        if (forced && *forced) {
            candidates.push_back(forced);
        }
        else {
            wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
            if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), localeName, LOCALE_NAME_MAX_LENGTH, 0)) return;
            std::string code;
            for (const wchar_t* c = localeName; *c; ++c) code += static_cast<char>(*c);
            const size_t dash = code.find('-');
            const std::string primary = code.substr(0, dash);
            if (primary == "zh") {
                // Traditional Chinese regions prefer zh-TW, otherwise Simplified Chinese.
                if (code == "zh-TW" || code == "zh-HK" || code == "zh-MO" || code.find("Hant") != std::string::npos) candidates.push_back("zh-TW");
                candidates.push_back("zh-CN");
            }
            else {
                candidates.push_back(code);
                if (dash != std::string::npos) candidates.push_back(primary);
            }
        }
        for (const std::string& name : candidates) {
            std::wstring file = dir;
            for (char c : name) file += static_cast<wchar_t>(c);
            file += L".lang";
            std::string data;
            if (readFileText(file, data)) {
                parseLanguageFile(data, out);
                return;
            }
        }
    }

    std::string tr(const char* key) {
        LanguageTable& lt = languageTable();
        if (!lt.loaded) {
            lt.loaded = true;
            loadLanguage(lt.table);
        }
        const auto it = lt.table.find(key);
        if (it != lt.table.end() && !it->second.empty()) return it->second;
        for (const DefaultText& d : kDefaultTexts)
            if (strcmp(d.key, key) == 0) return d.text;
        return key;
    }

    std::wstring trw(const char* key) {
        pfc::stringcvt::string_wide_from_utf8 wide(tr(key).c_str());
        return std::wstring(wide.get_ptr());
    }

    // tr() with {0} and {1} replaced.
    std::string trf(const char* key, const std::string& a0, const std::string& a1 = std::string()) {
        std::string text = tr(key);
        const std::string args[2] = { a0, a1 };
        for (int n = 0; n < 2; ++n) {
            const std::string token = "{" + std::to_string(n) + "}";
            for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + args[n].size()))
                text.replace(pos, token.size(), args[n]);
        }
        return text;
    }

    // DPI-aware fonts that match foobar2000's own preferences tree. The stock DEFAULT_GUI_FONT
    // never scales, which made text tiny next to the scaled layout on high-DPI (4K) screens, and
    // a generic system font came out larger than the tree.
    UINT uiDpiOf(HWND wnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static const auto fn = []() -> GetDpiForWindowFn {
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
            }();
        if (fn && wnd) { const UINT dpi = fn(wnd); if (dpi) return dpi; }
        return 96;
    }

    BOOL CALLBACK findTreeProc(HWND wnd, LPARAM lp) {
        wchar_t cls[32] = {};
        GetClassNameW(wnd, cls, 32);
        if (wcscmp(cls, L"SysTreeView32") == 0) {
            *reinterpret_cast<HWND*>(lp) = wnd;
            return FALSE;
        }
        return TRUE;
    }

    // Base UI font: the font of the preferences tree in the same window (so the page matches
    // it), otherwise the stock dialog font. Returns the DPI the font's height belongs to.
    UINT baseUiFont(HWND ref, LOGFONTW& lf) {
        lf = LOGFONTW{};
        HWND root = ref ? GetAncestor(ref, GA_ROOT) : nullptr;
        if (root) {
            HWND tree = nullptr;
            EnumChildWindows(root, findTreeProc, reinterpret_cast<LPARAM>(&tree));
            if (tree) {
                HFONT f = reinterpret_cast<HFONT>(SendMessageW(tree, WM_GETFONT, 0, 0));
                if (f && GetObjectW(f, sizeof(lf), &lf) == (int)sizeof(lf)) return uiDpiOf(ref);
            }
        }
        lf = LOGFONTW{};
        GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
        HDC dc = GetDC(nullptr);
        const int sysDpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        return sysDpi > 0 ? (UINT)sysDpi : 96;
    }

    // Explicit dark colors for edit boxes and list boxes. Relying only on foobar2000's dark-mode
    // hooks left black text on a dark background where they are not fully supported (Wine).
    constexpr COLORREF kDarkEditText = RGB(240, 240, 240);
    constexpr COLORREF kDarkEditBack = RGB(45, 45, 45);


    HBRUSH darkEditBrush() {
        static HBRUSH brush = CreateSolidBrush(kDarkEditBack);
        return brush;
    }

    HFONT createMonoFont(int lfHeight) {
        return CreateFontW(lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }

    COLORREF contrastingTextColor(COLORREF background) {
        const int r = GetRValue(background);
        const int g = GetGValue(background);
        const int b = GetBValue(background);

        const int brightness = (r * 299 + g * 587 + b * 114) / 1000;

        return brightness < 128
            ? RGB(240, 240, 240)
            : RGB(0, 0, 0);
    }

    // Modal Yes/No or OK box that follows foobar2000's dark mode (MessageBox does not).
    // Returns IDYES / IDNO for a Yes/No box and IDOK for an OK box.
    class themed_message {
    public:
        static int show(HWND owner, const char* title, const std::string& text, bool yesNo, bool defaultNo, LPCWSTR icon) {
            themed_message box(owner, title, text, yesNo, defaultNo, icon);
            return box.run();
        }

    private:
        themed_message(HWND owner, const char* title, const std::string& text, bool yesNo, bool defaultNo, LPCWSTR icon) :
            m_owner(owner), m_yesNo(yesNo), m_defaultNo(defaultNo), m_icon(icon),
            m_title(pfc::stringcvt::string_wide_from_utf8(title).get_ptr()),
            m_text(pfc::stringcvt::string_wide_from_utf8(text.c_str()).get_ptr()),
            m_brDark(CreateSolidBrush(RGB(32, 32, 32))) {
        }

        ~themed_message() {
            if (m_brDark) DeleteObject(m_brDark);
        }

        themed_message(const themed_message&) = delete;
        themed_message& operator=(const themed_message&) = delete;

        static UINT dpiOf(HWND wnd) {
            using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
            static const auto fn = []() -> GetDpiForWindowFn {
                const HMODULE user32 = GetModuleHandleW(L"user32.dll");
                return user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
                }();
            if (fn && wnd) { const UINT dpi = fn(wnd); if (dpi) return dpi; }
            return 96;
        }

        HWND addChild(LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, m_wnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
            return c;
        }

        int run() {
            static const bool registered = [] {
                WNDCLASSW wc{};
                wc.lpfnWndProc = proc;
                wc.hInstance = core_api::get_my_instance();
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.lpszClassName = L"foo_tftags_message";
                return RegisterClassW(&wc) != 0;
                }();
            (void)registered;

            const UINT dpi = dpiOf(m_owner ? m_owner : GetDesktopWindow());
            auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
            const int margin = S(14), gap = S(12), textW = S(360), btnW = S(84), btnH = S(26);
            const int iconSz = S(32);
            {
                LOGFONTW baseLf{};
                const UINT baseDpi = baseUiFont(m_owner, baseLf);
                baseLf.lfHeight = MulDiv(baseLf.lfHeight, (int)dpi, (int)baseDpi);
                m_font = CreateFontIndirectW(&baseLf);
            }

            int textH = 0;
            {
                HDC dc = GetDC(nullptr);
                HGDIOBJ old = SelectObject(dc, m_font);
                RECT rc{ 0, 0, textW, 0 };
                DrawTextW(dc, m_text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                textH = rc.bottom;
                SelectObject(dc, old);
                ReleaseDC(nullptr, dc);
            }
            const int contentH = (std::max)(textH, iconSz);
            const int clientW = margin + iconSz + gap + textW + margin;
            const int clientH = margin + contentH + gap + btnH + margin;

            const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
            const DWORD exStyle = WS_EX_DLGMODALFRAME;
            RECT wr{ 0, 0, clientW, clientH };
            AdjustWindowRectEx(&wr, style, FALSE, exStyle);
            const int w = wr.right - wr.left, h = wr.bottom - wr.top;

            RECT area{};
            if (!m_owner || !GetWindowRect(m_owner, &area)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            const int x = area.left + ((area.right - area.left) - w) / 2;
            const int y = area.top + ((area.bottom - area.top) - h) / 2;

            m_wnd = CreateWindowExW(exStyle, L"foo_tftags_message", m_title.c_str(), style, x, y, w, h,
                m_owner, nullptr, core_api::get_my_instance(), this);
            if (!m_wnd) {
                DeleteObject(m_font);
                return m_yesNo ? IDNO : IDOK;
            }

            HWND icon = addChild(L"STATIC", nullptr, SS_ICON, margin, margin, iconSz, iconSz, 0);
            SendMessageW(icon, STM_SETICON, reinterpret_cast<WPARAM>(LoadImageW(nullptr, m_icon, IMAGE_ICON, iconSz, iconSz, LR_SHARED)), 0);
            addChild(L"STATIC", m_text.c_str(), SS_LEFT | SS_NOPREFIX, margin + iconSz + gap,
                margin + (contentH - textH) / 2, textW, textH, 0);

            const int btnY = clientH - margin - btnH;
            const int lastX = clientW - margin - btnW;
            HWND defButton = nullptr;
            if (m_yesNo) {
                HWND yes = addChild(L"BUTTON", trw("btn_yes").c_str(), WS_TABSTOP | (m_defaultNo ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON),
                    lastX - S(8) - btnW, btnY, btnW, btnH, IDYES);
                HWND no = addChild(L"BUTTON", trw("btn_no").c_str(), WS_TABSTOP | (m_defaultNo ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                    lastX, btnY, btnW, btnH, IDNO);
                defButton = m_defaultNo ? no : yes;
            }
            else {
                defButton = addChild(L"BUTTON", trw("btn_ok").c_str(), WS_TABSTOP | BS_DEFPUSHBUTTON, lastX, btnY, btnW, btnH, IDOK);
            }
            m_dark.AddDialogWithControls(m_wnd);

            if (m_owner) EnableWindow(m_owner, FALSE);
            ShowWindow(m_wnd, SW_SHOW);
            SetForegroundWindow(m_wnd);
            SetFocus(defButton);

            MSG msg;
            while (!m_done) {
                const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                if (got <= 0) {
                    if (got == 0) PostQuitMessage((int)msg.wParam);
                    break;
                }
                if (!IsDialogMessageW(m_wnd, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            if (m_owner) EnableWindow(m_owner, TRUE);
            DestroyWindow(m_wnd);
            m_wnd = nullptr;
            DeleteObject(m_font);
            m_font = nullptr;
            if (m_owner) SetForegroundWindow(m_owner);
            return m_result ? m_result : (m_yesNo ? IDNO : IDOK);
        }

        static LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            auto* self = reinterpret_cast<themed_message*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE) {
                self = static_cast<themed_message*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self ? self->handle(wnd, msg, wp, lp) : DefWindowProcW(wnd, msg, wp, lp);
        }

        LRESULT handle(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            const bool dark = m_dark;
            switch (msg) {
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(wnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
                return 1;
            }
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wp);

                HBRUSH brush = dark
                    ? m_brDark
                    : GetSysColorBrush(COLOR_BTNFACE);

                const COLORREF background = dark
                    ? RGB(32, 32, 32)
                    : GetSysColor(COLOR_BTNFACE);

                SetTextColor(dc, contrastingTextColor(background));
                SetBkMode(dc, TRANSPARENT);

                return reinterpret_cast<LRESULT>(brush);
            }
            case WM_COMMAND: {
                int id = LOWORD(wp);
                if (id == IDCANCEL) id = m_yesNo ? IDNO : IDOK;
                else if (id == IDOK && m_yesNo) id = m_defaultNo ? IDNO : IDYES;
                if (id == IDYES || id == IDNO || id == IDOK) {
                    m_result = id;
                    m_done = true;
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                m_result = m_yesNo ? IDNO : IDOK;
                m_done = true;
                return 0;
            }
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        HWND m_owner;
        bool m_yesNo, m_defaultNo;
        LPCWSTR m_icon;
        std::wstring m_title, m_text;
        HBRUSH m_brDark;
        HWND m_wnd = nullptr;
        HFONT m_font = nullptr;
        int m_result = 0;
        bool m_done = false;
        fb2k::CCoreDarkModeHooks m_dark;
    };

    // ---------------------------------------------------------------- large script editor
    std::string getWindowText(HWND w) {
        const int len = GetWindowTextLengthW(w);
        std::wstring text((size_t)len + 1, L'\0');
        GetWindowTextW(w, &text[0], len + 1);
        text.resize((size_t)len);
        pfc::stringcvt::string_utf8_from_wide utf8(text.c_str());
        return std::string(utf8.get_ptr());
    }

    void setWindowText(HWND w, const std::string& text) {
        pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
        SetWindowTextW(w, wide.get_ptr());
    }

    // Edit controls need CR LF line breaks.
    std::string withCrLf(const std::string& text) {
        std::string out;
        out.reserve(text.size() + 16);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) out += '\r';
            out += text[i];
        }
        return out;
    }

    // Resizable modal window for long scripts, with its own live preview. The preview text comes
    // from a callback so the editor does not need to know how scripts are evaluated.
    class script_editor {
    public:
        using PreviewFn = std::function<std::string(const std::string&)>;

        // Returns true when OK was pressed; text then holds the edited script.
        static bool show(HWND owner, const std::string& title, std::string& text, PreviewFn preview) {
            script_editor box(owner, title, text, std::move(preview));
            const bool accepted = box.run();
            if (accepted) text = box.m_resultText;
            return accepted;
        }

    private:
        static constexpr int ID_EDIT = 101;
        static constexpr int ID_PREVIEW = 102;
        static constexpr UINT_PTR TIMER_PREVIEW = 1;

        script_editor(HWND owner, const std::string& title, const std::string& text, PreviewFn preview) :
            m_owner(owner), m_initial(withCrLf(text)), m_preview(std::move(preview)),
            m_title(pfc::stringcvt::string_wide_from_utf8(title.c_str()).get_ptr()),
            m_brDark(CreateSolidBrush(RGB(32, 32, 32))) {
        }

        ~script_editor() {
            if (m_brDark) DeleteObject(m_brDark);
        }

        script_editor(const script_editor&) = delete;
        script_editor& operator=(const script_editor&) = delete;

        static LRESULT CALLBACK EditProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
            if (msg == WM_GETDLGCODE) return DefSubclassProc(wnd, msg, wp, lp) | DLGC_WANTTAB;
            if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
                SendMessageW(wnd, EM_SETSEL, 0, -1);
                return 0;
            }
            if (msg == WM_CHAR && wp == 1) return 0;
            if (msg == WM_NCDESTROY) RemoveWindowSubclass(wnd, EditProc, id);
            return DefSubclassProc(wnd, msg, wp, lp);
        }

        HWND addChild(LPCWSTR cls, LPCWSTR text, DWORD style, DWORD exStyle, int id, HFONT font) {
            HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, m_wnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return c;
        }

        int S(int v) const { return MulDiv(v, (int)m_dpi, 96); }

        bool run() {
            static const bool registered = [] {
                WNDCLASSW wc{};
                wc.lpfnWndProc = proc;
                wc.hInstance = core_api::get_my_instance();
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.lpszClassName = L"foo_tftags_editor";
                return RegisterClassW(&wc) != 0;
                }();
            (void)registered;

            m_dpi = uiDpiOf(m_owner ? m_owner : GetDesktopWindow());
            LOGFONTW lf{};
            const UINT baseDpi = baseUiFont(m_owner, lf);
            lf.lfHeight = MulDiv(lf.lfHeight, (int)m_dpi, (int)baseDpi);
            m_font = CreateFontIndirectW(&lf);
            m_mono = createMonoFont(lf.lfHeight);

            RECT work{};
            MONITORINFO mi{ sizeof(mi) };
            if (m_owner && GetMonitorInfoW(MonitorFromWindow(m_owner, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
            else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            const int w = (std::min)(S(880), (int)(work.right - work.left) - S(40));
            const int h = (std::min)(S(660), (int)(work.bottom - work.top) - S(40));
            RECT anchor = work;
            if (m_owner) GetWindowRect(m_owner, &anchor);
            const int x = (std::max)((int)work.left, (int)(anchor.left + ((anchor.right - anchor.left) - w) / 2));
            const int y = (std::max)((int)work.top, (int)(anchor.top + ((anchor.bottom - anchor.top) - h) / 2));

            m_wnd = CreateWindowExW(0, L"foo_tftags_editor", m_title.c_str(),
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
                x, y, w, h, m_owner, nullptr, core_api::get_my_instance(), this);
            if (!m_wnd) {
                DeleteObject(m_font);
                DeleteObject(m_mono);
                return false;
            }

            pfc::stringcvt::string_wide_from_utf8 initial(m_initial.c_str());
            m_edit = addChild(L"EDIT", initial.get_ptr(),
                WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
                WS_EX_CLIENTEDGE, ID_EDIT, m_mono);
            SendMessageW(m_edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
            SetWindowSubclass(m_edit, EditProc, 1, 0);
            m_previewLabel = addChild(L"STATIC", trw("preview_label").c_str(), SS_LEFT | SS_NOPREFIX, 0, 0, m_font);
            m_previewBox = addChild(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                WS_EX_CLIENTEDGE, ID_PREVIEW, m_mono);
            m_ok = addChild(L"BUTTON", trw("btn_ok").c_str(), WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDOK, m_font);
            m_cancel = addChild(L"BUTTON", trw("btn_cancel").c_str(), WS_TABSTOP | BS_PUSHBUTTON, 0, IDCANCEL, m_font);
            m_dark.AddDialogWithControls(m_wnd);
            layout();
            updatePreview();

            if (m_owner) EnableWindow(m_owner, FALSE);
            ShowWindow(m_wnd, SW_SHOW);
            SetForegroundWindow(m_wnd);
            SetFocus(m_edit);

            MSG msg;
            while (!m_done) {
                const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                if (got <= 0) {
                    if (got == 0) PostQuitMessage((int)msg.wParam);
                    break;
                }
                if (!IsDialogMessageW(m_wnd, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            KillTimer(m_wnd, TIMER_PREVIEW);
            if (m_owner) EnableWindow(m_owner, TRUE);
            DestroyWindow(m_wnd);
            m_wnd = nullptr;
            DeleteObject(m_font);
            DeleteObject(m_mono);
            if (m_owner) SetForegroundWindow(m_owner);
            return m_accepted;
        }

        void layout() {
            RECT rc{};
            GetClientRect(m_wnd, &rc);
            const int w = rc.right, h = rc.bottom;
            const int margin = S(12), gap = S(8), btnW = S(96), btnH = S(28), labelH = S(20), previewH = S(120);
            const int btnY = h - margin - btnH;
            MoveWindow(m_cancel, w - margin - btnW, btnY, btnW, btnH, TRUE);
            MoveWindow(m_ok, w - margin - 2 * btnW - gap, btnY, btnW, btnH, TRUE);
            const int previewY = btnY - gap - previewH;
            MoveWindow(m_previewBox, margin, previewY, w - 2 * margin, previewH, TRUE);
            const int labelY = previewY - S(2) - labelH;
            MoveWindow(m_previewLabel, margin, labelY, w - 2 * margin, labelH, TRUE);
            MoveWindow(m_edit, margin, margin, w - 2 * margin, (std::max)(S(60), labelY - gap - margin), TRUE);
        }

        void updatePreview() {
            if (!m_preview) return;
            setWindowText(m_previewBox, withCrLf(m_preview(getWindowText(m_edit))));
        }

        // Cancel / close / Esc: ask before throwing away edits.
        void tryCancel() {
            if (getWindowText(m_edit) != m_initial) {
                if (themed_message::show(m_wnd, "TF Tags", tr("editor_discard"), true, true, IDI_QUESTION) != IDYES) return;
            }
            m_done = true;
        }

        static LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            auto* self = reinterpret_cast<script_editor*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE) {
                self = static_cast<script_editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self ? self->handle(wnd, msg, wp, lp) : DefWindowProcW(wnd, msg, wp, lp);
        }

        LRESULT handle(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            const bool dark = m_dark;
            switch (msg) {
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(wnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
                return 1;
            }
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wp);

                HBRUSH brush = dark
                    ? m_brDark
                    : GetSysColorBrush(COLOR_BTNFACE);

                const COLORREF background = dark
                    ? RGB(32, 32, 32)
                    : GetSysColor(COLOR_BTNFACE);

                SetTextColor(dc, contrastingTextColor(background));
                SetBkMode(dc, TRANSPARENT);

                return reinterpret_cast<LRESULT>(brush);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                HDC dc = reinterpret_cast<HDC>(wp);

                const COLORREF back = dark
                    ? kDarkEditBack
                    : GetSysColor(COLOR_WINDOW);

                SetTextColor(dc, dark
                    ? kDarkEditText
                    : GetSysColor(COLOR_WINDOWTEXT));

                SetBkColor(dc, back);

                return reinterpret_cast<LRESULT>(
                    dark ? darkEditBrush() : GetSysColorBrush(COLOR_WINDOW)
                    );
            }
            case WM_SIZE:
                if (m_edit && wp != SIZE_MINIMIZED) layout();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lp);
                info->ptMinTrackSize.x = S(520);
                info->ptMinTrackSize.y = S(380);
                return 0;
            }
            case WM_COMMAND: {
                const int id = LOWORD(wp);
                if (id == IDOK) {
                    m_resultText = getWindowText(m_edit);
                    m_accepted = true;
                    m_done = true;
                    return 0;
                }
                if (id == IDCANCEL) { tryCancel(); return 0; }
                if (id == ID_EDIT && HIWORD(wp) == EN_CHANGE) {
                    SetTimer(wnd, TIMER_PREVIEW, 300, nullptr);
                    return 0;
                }
                break;
            }
            case WM_TIMER:
                if (wp == TIMER_PREVIEW) {
                    KillTimer(wnd, TIMER_PREVIEW);
                    updatePreview();
                    return 0;
                }
                break;
            case WM_CLOSE:
                tryCancel();
                return 0;
            }
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        HWND m_owner;
        std::string m_initial;
        PreviewFn m_preview;
        std::wstring m_title;
        HBRUSH m_brDark;
        HWND m_wnd = nullptr, m_edit = nullptr, m_previewLabel = nullptr, m_previewBox = nullptr, m_ok = nullptr, m_cancel = nullptr;
        HFONT m_font = nullptr, m_mono = nullptr;
        UINT m_dpi = 96;
        std::string m_resultText;
        bool m_accepted = false;
        bool m_done = false;
        fb2k::CCoreDarkModeHooks m_dark;
    };

    // Tells playlists and panels that the displayed values may have changed (main thread only).
    void refreshViews() {
        try {
            metadb_handle_list items;
            playlist_manager::get()->activeplaylist_get_all_items(items);
            metadb_handle_ptr playing;
            if (playback_control::get()->get_now_playing(playing) && playing.is_valid()) items.add_item(playing);
            if (items.get_count() > 0) metadb_io::get()->dispatch_refresh(items);
        }
        catch (...) {
        }
    }

    // ---------------------------------------------------------------- preferences

    constexpr int ID_LIST = 2001;
    constexpr int ID_NAME = 2002;
    constexpr int ID_SCRIPT = 2003;
    constexpr int ID_PREVIEW_TIMER = 2004;  // delayed preview while typing
    constexpr int ID_RESULT = 2005;
    constexpr int ID_LANG = 2006;
    constexpr int ID_EDITOR = 2007;
    constexpr int ID_IMPORT = 2008;
    constexpr int ID_EXPORT = 2009;
    constexpr int ID_TABLE = 2010;
    constexpr int ID_TABLEDIR = 2011;
    constexpr int ID_ADD = 2012;
    constexpr int ID_REMOVE = 2013;

    class tftags_preferences_instance : public preferences_page_instance {
    public:
        tftags_preferences_instance(HWND parent, preferences_page_callback::ptr callback)
            : m_callback(callback), m_wnd(nullptr), m_ready(false), m_current(0),
            m_list(nullptr), m_nameLabel(nullptr), m_hint(nullptr), m_field(nullptr), m_scriptLabel(nullptr),
            m_name(nullptr), m_script(nullptr), m_result(nullptr), m_note(nullptr), m_langLabel(nullptr), m_langCombo(nullptr), m_previewLabel(nullptr), m_scriptButton(nullptr), m_importButton(nullptr), m_exportButton(nullptr), m_tableLabel(nullptr), m_tableCombo(nullptr), m_tableFolder(nullptr), m_addButton(nullptr), m_removeButton(nullptr),
            m_uiFont(nullptr), m_monoFont(nullptr), m_brDark(CreateSolidBrush(RGB(32, 32, 32))) {

            HINSTANCE hInstance = core_api::get_my_instance();
            WNDCLASSW wc{};
            if (!GetClassInfoW(hInstance, L"TfTagsPreferencesPage", &wc)) {
                wc.lpfnWndProc = WindowProc;
                wc.hInstance = hInstance;
                wc.lpszClassName = L"TfTagsPreferencesPage";
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = nullptr;
                RegisterClassW(&wc);
            }

            RECT rcParent{};
            GetClientRect(parent, &rcParent);
            const int initW = (rcParent.right > 0) ? rcParent.right : 640;
            const int initH = (rcParent.bottom > 0) ? rcParent.bottom : 520;

            m_wnd = CreateWindowExW(0, L"TfTagsPreferencesPage", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                0, 0, initW, initH, parent, nullptr, hInstance, this);
            if (!m_wnd) return;

            m_saved = savedSlots();
            for (const SlotData& slot : m_saved) {
                m_ids.push_back(slot.id);
                m_names.push_back(slot.name);
                m_scripts.push_back(withCrLf(slot.script));
                m_tables.push_back(slot.table);
                m_nextId = (std::max)(m_nextId, slot.id + 1);
            }

            m_baseDpi = baseUiFont(m_wnd, m_baseLf);
            rebuildFonts();

            m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), hInstance, nullptr);
            setFont(m_list, uiFont());
            m_addButton = CreateWindowExW(0, L"BUTTON", trw("add_slot").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ADD)), hInstance, nullptr);
            setFont(m_addButton, uiFont());
            m_removeButton = CreateWindowExW(0, L"BUTTON", trw("remove_slot").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REMOVE)), hInstance, nullptr);
            setFont(m_removeButton, uiFont());

            m_nameLabel = addLabel(trw("name_label").c_str());
            m_name = addEdit(ID_NAME, false);
            m_hint = addLabel(L"");
            // Read-only edit instead of a label so the field name can be selected and copied.
            m_field = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                0, 0, 10, 10, m_wnd, nullptr, hInstance, nullptr);
            setFont(m_field, uiFont());
            m_scriptLabel = addLabel(trw("script_label").c_str());
            m_script = addEdit(ID_SCRIPT, true);
            if (m_monoFont) setFont(m_script, m_monoFont);
            m_scriptButton = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDITOR)), hInstance, nullptr);
            setFont(m_scriptButton, uiFont());
            m_previewLabel = addLabel(trw("preview_label").c_str());
            m_result = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESULT)), hInstance, nullptr);
            setFont(m_result, uiFont());
            for (HWND edit : { m_name, m_script, m_result, m_field, m_list })
                if (edit) SetWindowSubclass(edit, EditSubclassProc, 1, 0);
            m_note = addLabel((trw("note_1") + L"\n" + trw("note_3")).c_str());
            m_langLabel = addLabel(trw("lang_label").c_str());
            m_langCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LANG)), hInstance, nullptr);
            setFont(m_langCombo, uiFont());
            m_langPending = savedLanguage();
            fillLanguageCombo();
            m_importButton = CreateWindowExW(0, L"BUTTON", trw("import_button").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_IMPORT)), hInstance, nullptr);
            setFont(m_importButton, uiFont());
            m_exportButton = CreateWindowExW(0, L"BUTTON", trw("export_button").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EXPORT)), hInstance, nullptr);
            setFont(m_exportButton, uiFont());
            m_tableLabel = addLabel(trw("table_label").c_str());
            m_tableCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TABLE)), hInstance, nullptr);
            setFont(m_tableCombo, uiFont());
            m_tableFolder = CreateWindowExW(0, L"BUTTON", trw("table_folder").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TABLEDIR)), hInstance, nullptr);
            setFont(m_tableFolder, uiFont());
            fillTableCombo();

            refreshList();
            loadCurrent();

            m_darkMode.AddDialogWithControls(m_wnd);
            m_ready = true;
            layoutControls(initW, initH);
        }

        ~tftags_preferences_instance() {
            if (m_uiFont) DeleteObject(m_uiFont);
            if (m_monoFont) DeleteObject(m_monoFont);
            if (m_brDark) DeleteObject(m_brDark);
        }

        t_uint32 get_state() override {
            t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
            if (m_wnd && hasChanges()) state |= preferences_state::changed;
            return state;
        }

        fb2k::hwnd_t get_wnd() override { return m_wnd; }

        void apply() override {
            if (!m_wnd) return;
            storeCurrent();
            const std::string cycle = findCycle(m_names, m_scripts);
            if (!cycle.empty()) {
                const std::string msg = trf("cycle_apply", cycle);
                themed_message::show(GetAncestor(m_wnd, GA_ROOT), "TF Tags", msg, false, false, IDI_WARNING);
                return;
            }
            const std::vector<std::string> newFields = fieldNames(m_names);
            std::string broken;
            for (size_t i = 0; i < m_scripts.size(); ++i) {
                const std::string flat = flattenScript(m_scripts[i]);
                titleformat_object::ptr obj;
                if (!flat.empty() && !titleformat_compiler::get()->compile(obj, flat.c_str()))
                    broken += "\n    %" + newFields[i] + "%";
            }
            if (!broken.empty()) {
                const std::string msg = trf("broken_scripts", broken);
                if (themed_message::show(GetAncestor(m_wnd, GA_ROOT), "TF Tags", msg, true, true, IDI_WARNING) != IDYES) return;
            }

            const std::vector<std::string> fieldsBefore = savedFieldNames();
            std::vector<SlotData> updated;
            for (size_t i = 0; i < m_ids.size(); ++i) {
                SlotData slot;
                slot.id = m_ids[i];
                slot.name = cleanName(m_names[i]);
                slot.script = stripCr(m_scripts[i]);
                slot.table = m_tables[i];
                m_names[i] = slot.name;
                updated.push_back(slot);
            }
            bool scriptsChanged = updated.size() != m_saved.size();
            for (const SlotData& slot : updated) {
                const auto old = std::find_if(m_saved.begin(), m_saved.end(), [&](const SlotData& o) { return o.id == slot.id; });
                if (old == m_saved.end() || old->script != slot.script || old->table != slot.table) scriptsChanged = true;
            }
            setSlots(updated);
            m_saved = updated;
            if (m_langPending != savedLanguage()) {
                g_language = m_langPending.c_str();
                LanguageTable& lt = languageTable();
                lt.table.clear();
                lt.loaded = false;
                applyTexts();
            }
            loadCurrent();
            refreshList();
            if (m_callback.is_valid()) m_callback->on_state_changed();
            if (scriptsChanged) refreshViews();

            // Field names are only read at startup, so an added, renamed or removed field needs a
            // restart. Settings are written on exit, so the slots survive it.
            if (savedFieldNames() != fieldsBefore) {
                HWND owner = m_wnd ? GetAncestor(m_wnd, GA_ROOT) : nullptr;
                const int answer = themed_message::show(owner, "TF Tags",
                    tr("restart_prompt"), true, false, IDI_QUESTION);
                if (answer == IDYES) {
                    fb2k::inMainThread([] {
                        standard_commands::run_main(standard_commands::guid_main_restart);
                        });
                }
            }
        }

        void reset() override {
            if (!m_wnd) return;
            m_ids.assign(1, m_nextId++);
            m_names.assign(1, "01");
            m_scripts.assign(1, std::string());
            m_tables.assign(1, std::string());
            m_current = 0;
            m_langPending.clear();
            fillLanguageCombo();
            loadCurrent();
            refreshList();
            if (m_callback.is_valid()) m_callback->on_state_changed();
        }

    private:
        // Edit controls do not handle Ctrl+A themselves.
        static LRESULT CALLBACK EditSubclassProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
            if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
                SendMessageW(wnd, EM_SETSEL, 0, -1);
                return 0;
            }
            if (msg == WM_CHAR && wp == 1) return 0;
            if (msg == WM_LBUTTONUP) {
                // A click on the slot list, the tag name or the script asks the page for a preview.
                const LRESULT result = DefSubclassProc(wnd, msg, wp, lp);
                const int ctl = GetDlgCtrlID(wnd);
                HWND parent = GetParent(wnd);
                if (parent && (ctl == ID_LIST || ctl == ID_NAME || ctl == ID_SCRIPT))
                    PostMessageW(parent, WM_APP + 1, (WPARAM)ctl, 0);
                return result;
            }
            if (msg == WM_NCDESTROY) RemoveWindowSubclass(wnd, EditSubclassProc, id);
            return DefSubclassProc(wnd, msg, wp, lp);
        }

        static UINT dpiForWindow(HWND wnd) {
            using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
            static const auto fn = []() -> GetDpiForWindowFn {
                const HMODULE user32 = GetModuleHandleW(L"user32.dll");
                return user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
                }();
            if (fn && wnd) { const UINT dpi = fn(wnd); if (dpi) return dpi; }
            return 96;
        }
        int S(int v) const { return MulDiv(v, (int)dpiForWindow(m_wnd), 96); }

        HGDIOBJ uiFont() const { return m_uiFont ? static_cast<HGDIOBJ>(m_uiFont) : GetStockObject(DEFAULT_GUI_FONT); }

        // Lists the tables folder; the current slot's table stays selected even if its file is gone.
        void fillTableCombo() {
            if (!m_tableCombo || m_current >= m_tables.size()) return;
            const std::string wanted = m_tables[m_current];
            SendMessageW(m_tableCombo, CB_RESETCONTENT, 0, 0);
            m_tableNames.assign(1, std::string());
            auto add = [&](const std::string& text) {
                pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
                SendMessageW(m_tableCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.get_ptr()));
                };
            add(tr("table_none"));
            for (const std::string& name : listTables()) {
                m_tableNames.push_back(name);
                add(name);
            }
            size_t pick = 0;
            for (size_t i = 0; i < m_tableNames.size(); ++i)
                if (m_tableNames[i] == wanted) pick = i;
            if (pick == 0 && !wanted.empty()) {
                m_tableNames.push_back(wanted);
                add(wanted);
                pick = m_tableNames.size() - 1;
            }
            SendMessageW(m_tableCombo, CB_SETCURSEL, pick, 0);
        }

        void openTablesFolder() {
            const std::wstring dir = tablesDirectory(true);
            if (!dir.empty()) ShellExecuteW(GetAncestor(m_wnd, GA_ROOT), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        static std::string savedLanguage() {
            const char* v = g_language.get();
            return v ? v : "";
        }

        void fillLanguageCombo() {
            SendMessageW(m_langCombo, CB_RESETCONTENT, 0, 0);
            m_langCodes.assign(1, std::string());
            auto add = [&](const std::string& text) {
                pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
                SendMessageW(m_langCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.get_ptr()));
                };
            add(tr("lang_auto"));
            for (const LanguageEntry& e : listLanguages()) {
                m_langCodes.push_back(e.code);
                add(e.name);
            }
            size_t pick = 0;
            for (size_t i = 0; i < m_langCodes.size(); ++i)
                if (m_langCodes[i] == m_langPending) pick = i;
            m_langPending = m_langCodes[pick];
            SendMessageW(m_langCombo, CB_SETCURSEL, pick, 0);
        }

        // Re-applies all texts after the language changed.
        void applyTexts() {
            writeWindow(m_nameLabel, tr("name_label"));
            writeWindow(m_scriptLabel, tr("script_label"));
            writeWindow(m_langLabel, tr("lang_label"));
            writeWindow(m_previewLabel, tr("preview_label"));
            writeWindow(m_importButton, tr("import_button"));
            writeWindow(m_tableLabel, tr("table_label"));
            writeWindow(m_tableFolder, tr("table_folder"));
            writeWindow(m_addButton, tr("add_slot"));
            writeWindow(m_removeButton, tr("remove_slot"));
            fillTableCombo();
            writeWindow(m_exportButton, tr("export_button"));
            writeWindow(m_note, tr("note_1") + "\n" + tr("note_3"));
            fillLanguageCombo();
            updateHint();
            RECT rc{};
            GetClientRect(m_wnd, &rc);
            layoutControls(rc.right, rc.bottom);
        }

        // Builds the UI and script fonts for the window's current DPI from the base font (the
        // preferences tree's font). The caller releases the previous fonts.
        void rebuildFonts() {
            LOGFONTW lf = m_baseLf;
            lf.lfHeight = MulDiv(m_baseLf.lfHeight, (int)dpiForWindow(m_wnd), (int)m_baseDpi);
            m_uiFont = CreateFontIndirectW(&lf);
            m_monoFont = createMonoFont(lf.lfHeight);
        }

        // The page's DPI changed (monitor change or scale change): rebuild the fonts and layout.
        void applyDpi() {
            if (!m_ready || !m_wnd) return;
            HFONT oldUi = m_uiFont, oldMono = m_monoFont;
            rebuildFonts();
            for (HWND c : { m_list, m_nameLabel, m_name, m_hint, m_field, m_scriptLabel, m_result, m_note, m_langLabel, m_langCombo, m_previewLabel, m_scriptButton, m_importButton, m_exportButton, m_tableLabel, m_tableCombo, m_tableFolder, m_addButton, m_removeButton })
                if (c) setFont(c, uiFont());
            if (m_script) setFont(m_script, m_monoFont ? static_cast<HGDIOBJ>(m_monoFont) : uiFont());
            if (oldUi) DeleteObject(oldUi);
            if (oldMono) DeleteObject(oldMono);
            RECT rc{};
            GetClientRect(m_wnd, &rc);
            layoutControls(rc.right, rc.bottom);
        }

        static void setFont(HWND w, HGDIOBJ f) { SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE); }

        HWND addLabel(const wchar_t* text) const {
            HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0, 0, 10, 10, m_wnd, nullptr, core_api::get_my_instance(), nullptr);
            setFont(c, uiFont());
            return c;
        }

        HWND addEdit(int id, bool multiline) const {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            style |= multiline ? (WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : ES_AUTOHSCROLL;
            HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
            setFont(c, uiFont());
            return c;
        }

        static std::string readWindow(HWND w) {
            const int len = GetWindowTextLengthW(w);
            std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(w, wide.data(), len + 1);
            wide.resize(len);
            pfc::stringcvt::string_utf8_from_wide utf8(wide.c_str());
            return std::string(utf8.get_ptr());
        }

        static void writeWindow(HWND w, const std::string& s) {
            pfc::stringcvt::string_wide_from_utf8 wide(s.c_str());
            SetWindowTextW(w, wide.get_ptr());
        }

        void storeCurrent() {
            if (m_current >= m_names.size()) return;
            m_names[m_current] = readWindow(m_name);
            m_scripts[m_current] = readWindow(m_script);
            if (m_tableCombo && !m_tableNames.empty()) {
                const int sel = (int)SendMessageW(m_tableCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && (size_t)sel < m_tableNames.size()) m_tables[m_current] = m_tableNames[(size_t)sel];
            }
        }

        void updateHint() {
            writeWindow(m_hint, tr("use_in_tf"));
            writeWindow(m_field, "%" + fieldNameFor(m_current, m_names) + "%");
        }

        void loadCurrent() {
            m_loading = true;
            writeWindow(m_name, m_names[m_current]);
            writeWindow(m_script, m_scripts[m_current]);
            m_loading = false;
            fillTableCombo();
            updateHint();
        }

        void refreshList() {
            const int top = (int)SendMessageW(m_list, LB_GETTOPINDEX, 0, 0);
            SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
            SendMessageW(m_list, LB_RESETCONTENT, 0, 0);
            const std::vector<std::string> fields = fieldNames(m_names);
            const size_t digits = (std::max)((size_t)2, std::to_string(m_names.size()).size());
            for (size_t i = 0; i < m_names.size(); ++i) {
                std::string number = std::to_string(i + 1);
                while (number.size() < digits) number.insert(number.begin(), '0');
                const std::string text = number + "   %" + fields[i] + "%";
                pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
                SendMessageW(m_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.get_ptr()));
            }
            SendMessageW(m_list, LB_SETCURSEL, m_current < m_names.size() ? m_current : 0, 0);
            if (top > 0) SendMessageW(m_list, LB_SETTOPINDEX, top, 0);
            SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(m_list, nullptr, TRUE);
        }

        bool hasChanges() {
            storeCurrent();
            if (m_langPending != savedLanguage()) return true;
            if (m_ids.size() != m_saved.size()) return true;
            for (size_t i = 0; i < m_ids.size(); ++i) {
                const SlotData& saved = m_saved[i];
                if (m_ids[i] != saved.id) return true;
                if (cleanName(m_names[i]) != cleanName(saved.name)) return true;
                if (stripCr(m_scripts[i]) != saved.script) return true;
                if (m_tables[i] != saved.table) return true;
            }
            return false;
        }

        // Next unused two-digit tag name: 01, 02, ...
        std::string defaultSlotName() const {
            for (size_t n = 1;; ++n) {
                std::string name = std::to_string(n);
                if (name.size() < 2) name.insert(name.begin(), '0');
                bool used = false;
                for (const std::string& existing : m_names)
                    if (cleanName(existing) == name) { used = true; break; }
                if (!used) return name;
            }
        }

        void addSlot() {
            storeCurrent();
            m_ids.push_back(m_nextId++);
            m_names.push_back(defaultSlotName());
            m_scripts.emplace_back();
            m_tables.emplace_back();
            m_current = m_names.size() - 1;
            refreshList();
            loadCurrent();
            if (m_callback.is_valid()) m_callback->on_state_changed();
            runTest();
            SetFocus(m_name);
            SendMessageW(m_name, EM_SETSEL, 0, -1);
        }

        void removeSlot() {
            storeCurrent();
            if (m_current >= m_names.size()) return;
            const std::string field = "%" + fieldNameFor(m_current, m_names) + "%";
            if (themed_message::show(GetAncestor(m_wnd, GA_ROOT), "TF Tags", trf("remove_confirm", field), true, true, IDI_QUESTION) != IDYES) return;
            const std::ptrdiff_t at = (std::ptrdiff_t)m_current;
            m_ids.erase(m_ids.begin() + at);
            m_names.erase(m_names.begin() + at);
            m_scripts.erase(m_scripts.begin() + at);
            m_tables.erase(m_tables.begin() + at);
            if (m_names.empty()) {  // there is always at least one slot
                m_ids.push_back(m_nextId++);
                m_names.push_back("01");
                m_scripts.emplace_back();
                m_tables.emplace_back();
            }
            if (m_current >= m_names.size()) m_current = m_names.size() - 1;
            refreshList();
            loadCurrent();
            if (m_callback.is_valid()) m_callback->on_state_changed();
            runTest();
        }

        // Evaluates a script for the selected track and returns the text for a preview box.
        // `script` replaces the current slot's script (other slots use their saved scripts).
        std::string previewFor(const std::string& script) {
            std::vector<std::string> scripts = m_scripts;
            if (m_current < scripts.size()) scripts[m_current] = script;
            const std::string cycle = findCycle(m_names, scripts);
            if (!cycle.empty()) return trf("cycle_test", cycle);

            // A selected track always wins, even while another track is playing: the active playlist
            // selection first, then the selection of any other panel (e.g. the library), then the
            // focused playlist row. The playing track is only used when none of these exist.
            metadb_handle_ptr playing;
            playback_control::get()->get_now_playing(playing);

            metadb_handle_ptr track;
            metadb_handle_list sel;
            playlist_manager::get()->activeplaylist_get_selected_items(sel);
            if (sel.get_count() == 0) ui_selection_manager::get()->get_selection(sel);
            if (sel.get_count() > 0) track = sel[0];
            else {
                metadb_handle_ptr focused;
                if (playlist_manager::get()->activeplaylist_get_focus_item_handle(focused) && focused.is_valid()) track = focused;
                else track = playing;
            }
            if (track.is_empty()) return tr("test_no_track");

            const std::string flat = flattenScript(script);
            titleformat_object::ptr obj;
            if (flat.empty() || !titleformat_compiler::get()->compile(obj, flat.c_str())) return tr("test_empty_script");
            const std::string& table = m_tables[m_current];
            if (!table.empty() && tableState(table) != 0) return trf("table_error", table);
            pfc::string8 out;
            formatSlot(track.get_ptr(), obj, table, out);
            std::string result = out.c_str();
            applyTable(table, result);
            return result.empty() ? tr("test_empty_result") : result;
        }

        // Fills the Preview box for the current slot.
        void runTest() {
            storeCurrent();
            writeWindow(m_result, withCrLf(previewFor(m_scripts[m_current])));
        }

        // ---- import / export of the tag names and scripts (UTF-8 text, key=value lines)
        static std::string escapeValue(const std::string& text) {
            std::string out;
            for (char c : text) {
                if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c != '\r') out += c;
            }
            return out;
        }

        static std::string slotKey(const char* prefix, size_t i) {
            std::string number = std::to_string(i + 1);
            if (number.size() < 2) number.insert(number.begin(), '0');
            return std::string(prefix) + "." + number;
        }

        bool pickSettingsFile(bool save, std::wstring& path) {
            std::wstring filter = trw("filter_settings");
            filter += L'\0';
            filter += L"*.tftags";
            filter += L'\0';
            filter += trw("filter_all");
            filter += L'\0';
            filter += L"*.*";
            filter += L'\0';
            filter += L'\0';
            const std::wstring title = trw(save ? "export_title" : "import_title");
            wchar_t buffer[MAX_PATH * 4] = {};
            if (save) wcscpy_s(buffer, L"foo_tftags_settings.tftags");
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetAncestor(m_wnd, GA_ROOT);
            ofn.lpstrFilter = filter.c_str();
            ofn.lpstrFile = buffer;
            ofn.nMaxFile = (DWORD)(sizeof(buffer) / sizeof(wchar_t));
            ofn.lpstrTitle = title.c_str();
            ofn.lpstrDefExt = L"tftags";
            ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY
                | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
            const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
            if (!ok) return false;
            path = buffer;
            return true;
        }

        // Exports what is in the page now, including changes that are not applied yet.
        void exportSettings() {
            storeCurrent();
            std::wstring path;
            if (!pickSettingsFile(true, path)) return;
            std::string data = "# TF Tags settings\n";
            data += "format=foo_tftags-settings-1\n";
            for (size_t i = 0; i < m_names.size(); ++i) {
                data += slotKey("name", i) + "=" + escapeValue(cleanName(m_names[i])) + "\n";
                data += slotKey("script", i) + "=" + escapeValue(m_scripts[i]) + "\n";
                data += slotKey("table", i) + "=" + escapeValue(m_tables[i]) + "\n";
            }
            FILE* f = nullptr;
            const bool ok = _wfopen_s(&f, path.c_str(), L"wb") == 0 && f
                && fwrite(data.data(), 1, data.size(), f) == data.size();
            if (f) fclose(f);
            if (!ok) themed_message::show(GetAncestor(m_wnd, GA_ROOT), "TF Tags", tr("export_failed"), false, false, IDI_WARNING);
        }

        struct ImportedSlot {
            std::string name, script, table;
        };

        // Merges the slots of a settings file into the page: a slot with the same tag name is
        // replaced, the others are added. Nothing takes effect until Apply is pressed.
        void importSettings() {
            std::wstring path;
            if (!pickSettingsFile(false, path)) return;
            HWND owner = GetAncestor(m_wnd, GA_ROOT);
            std::string data;
            if (!readFileText(path, data)) {
                themed_message::show(owner, "TF Tags", tr("import_failed"), false, false, IDI_WARNING);
                return;
            }
            std::unordered_map<std::string, std::string> table;
            parseLanguageFile(data, table);
            const auto format = table.find("format");
            if (format == table.end() || format->second != "foo_tftags-settings-1") {
                themed_message::show(owner, "TF Tags", tr("import_invalid"), false, false, IDI_WARNING);
                return;
            }

            std::map<unsigned long, ImportedSlot> found;  // by the number in the key
            for (const auto& entry : table) {
                const std::string& key = entry.first;
                const size_t dot = key.find('.');
                if (dot == std::string::npos) continue;
                const std::string field = key.substr(0, dot);
                if (field != "name" && field != "script" && field != "table") continue;
                char* end = nullptr;
                const unsigned long number = strtoul(key.c_str() + dot + 1, &end, 10);
                if (number == 0 || !end || *end != '\0') continue;
                ImportedSlot& slot = found[number];
                if (field == "name") slot.name = cleanName(entry.second);
                else if (field == "script") slot.script = withCrLf(entry.second);
                else slot.table = entry.second;
            }
            size_t usable = 0;
            for (const auto& entry : found)
                if (!entry.second.script.empty() || !entry.second.table.empty()) ++usable;
            if (usable == 0) {
                themed_message::show(owner, "TF Tags", tr("import_empty"), false, false, IDI_WARNING);
                return;
            }
            if (themed_message::show(owner, "TF Tags", tr("import_confirm"), true, false, IDI_QUESTION) != IDYES) return;

            storeCurrent();
            for (const auto& entry : found) {
                const ImportedSlot& slot = entry.second;
                if (slot.script.empty() && slot.table.empty()) continue;  // an unused slot of an older file
                size_t target = m_names.size();
                if (!slot.name.empty())
                    for (size_t i = 0; i < m_names.size(); ++i)
                        if (cleanName(m_names[i]) == slot.name) { target = i; break; }
                if (target == m_names.size()) {
                    m_ids.push_back(m_nextId++);
                    m_names.push_back(slot.name.empty() ? defaultSlotName() : slot.name);
                    m_scripts.emplace_back();
                    m_tables.emplace_back();
                }
                m_scripts[target] = slot.script;
                m_tables[target] = slot.table;
            }
            loadCurrent();
            refreshList();
            if (m_callback.is_valid()) m_callback->on_state_changed();
            runTest();
        }

        // Opens the large script editor ("..." button). OK copies the text back to the script box.
        void openEditor() {
            storeCurrent();
            std::string text = m_scripts[m_current];
            const std::string title = tr("editor_title") + " - %" + fieldNameFor(m_current, m_names) + "%";
            HWND owner = GetAncestor(m_wnd, GA_ROOT);
            if (script_editor::show(owner, title, text, [this](const std::string& script) { return previewFor(script); })) {
                writeWindow(m_script, text);
                storeCurrent();
                runTest();
            }
        }

        void layoutControls(int w, int h) {
            if (!m_ready) return;
            const int margin = S(12), gap = S(8), listW = S(190), rowH = S(24), labelH = S(18);
            const int resultH = S(56);
            // The footer note wraps on narrow pages and grows with the font size, so measure it.
            int noteH = S(52);
            {
                const int len = GetWindowTextLengthW(m_note);
                std::wstring text((size_t)len + 1, L'\0');
                GetWindowTextW(m_note, &text[0], len + 1);
                text.resize((size_t)len);
                HDC dc = GetDC(m_wnd);
                HGDIOBJ old = SelectObject(dc, uiFont());
                RECT rc{ 0, 0, (std::max)(S(100), w - 2 * margin), 0 };
                DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(dc, old);
                ReleaseDC(m_wnd, dc);
                noteH = (std::max)(noteH, (int)rc.bottom + S(4));
            }
            const int rightX = margin + listW + gap;
            const int rightW = (std::max)(S(120), w - rightX - margin);
            auto place = [](HWND c, int x, int y, int cw, int ch) {
                SetWindowPos(c, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
                };
            const int bottom = h - margin;
            // Language row at the very bottom, the footer note above it.
            auto textWidth = [&](HWND c) {
                const int len = GetWindowTextLengthW(c);
                std::wstring text((size_t)len + 1, L'\0');
                GetWindowTextW(c, &text[0], len + 1);
                text.resize((size_t)len);
                HDC dc = GetDC(m_wnd);
                HGDIOBJ old = SelectObject(dc, uiFont());
                SIZE sz{};
                GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
                SelectObject(dc, old);
                ReleaseDC(m_wnd, dc);
                return (int)sz.cx + S(4);
                };
            const int langLabelW = textWidth(m_langLabel);
            place(m_langLabel, margin, bottom - rowH + (rowH - labelH) / 2, langLabelW, labelH);
            const int impW = textWidth(m_importButton) + S(24), expW = textWidth(m_exportButton) + S(24);
            place(m_exportButton, w - margin - expW, bottom - rowH, expW, rowH);
            place(m_importButton, w - margin - expW - gap - impW, bottom - rowH, impW, rowH);
            const int comboRoom = w - margin - langLabelW - gap - (impW + gap + expW + gap) - margin;
            place(m_langCombo, margin + langLabelW + gap, bottom - rowH, (std::min)(S(260), (std::max)(S(100), comboRoom)), rowH + S(200));
            place(m_note, margin, bottom - rowH - gap - noteH, w - 2 * margin, noteH);
            const int contentBottom = bottom - rowH - gap - noteH - gap;
            const int halfW = (listW - gap) / 2;
            place(m_addButton, margin, contentBottom - rowH, halfW, rowH);
            place(m_removeButton, margin + halfW + gap, contentBottom - rowH, listW - halfW - gap, rowH);
            place(m_list, margin, margin, listW, (std::max)(S(60), contentBottom - margin - rowH - gap));

            int y = margin;
            place(m_nameLabel, rightX, y, rightW, labelH); y += labelH + S(2);
            place(m_name, rightX, y, (std::min)(rightW, S(260)), rowH);
            y += rowH + S(2);
            // The label and the selectable field share one row; the field shrinks on narrow pages.
            const int hintW = textWidth(m_hint);
            place(m_hint, rightX, y + (rowH - labelH) / 2, hintW, labelH);
            place(m_field, rightX + hintW + gap, y, (std::max)(S(80), (std::min)(rightW - hintW - gap, S(260))), rowH);
            y += rowH + gap;
            // Replacement table row: label, table drop-down, button that opens the folder.
            // The button keeps its place at the right edge; the label gives way before the
            // drop-down gets narrower than 90 px, so the three never overlap.
            const int folderW = textWidth(m_tableFolder) + S(24);
            int tableLabelW = textWidth(m_tableLabel);
            int tableComboW = rightW - tableLabelW - 2 * gap - folderW;
            if (tableComboW < S(90)) {
                tableComboW = S(90);
                tableLabelW = (std::max)(S(40), rightW - tableComboW - 2 * gap - folderW);
            }
            tableComboW = (std::min)(tableComboW, S(240));
            place(m_tableLabel, rightX, y + (rowH - labelH) / 2, tableLabelW, labelH);
            place(m_tableCombo, rightX + tableLabelW + gap, y, tableComboW, rowH + S(200));
            place(m_tableFolder, rightX + rightW - folderW, y, folderW, rowH);
            y += rowH + gap;
            // The "..." button opens the large editor; it sits at the right end of the label row.
            const int dotsW = S(34);
            place(m_scriptButton, rightX + rightW - dotsW, y, dotsW, rowH);
            place(m_scriptLabel, rightX, y + (rowH - labelH) / 2, (std::max)(S(40), rightW - dotsW - gap), labelH);
            y += rowH + S(2);
            const int lower = labelH + S(2) + resultH;
            const int scriptH = (std::max)(S(80), contentBottom - y - lower - gap);
            place(m_script, rightX, y, rightW, scriptH); y += scriptH + gap;
            place(m_previewLabel, rightX, y, rightW, labelH); y += labelH + S(2);
            place(m_result, rightX, y, rightW, (std::max)(S(30), contentBottom - y));
        }

        static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            auto* self = reinterpret_cast<tftags_preferences_instance*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE) {
                auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                self = static_cast<tftags_preferences_instance*>(cs->lpCreateParams);
                SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self) return self->handleMessage(wnd, msg, wp, lp);
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        LRESULT handleMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            const bool dark = m_darkMode;
            if (msg == WM_ERASEBKGND) {
                HDC dc = (HDC)wp;
                RECT rc{};
                GetClientRect(wnd, &rc);
                FillRect(dc, &rc, dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
                return 1;
            }
            if (msg == WM_CTLCOLORSTATIC) {
                HDC dc = reinterpret_cast<HDC>(wp);

                HBRUSH brush = dark
                    ? m_brDark
                    : GetSysColorBrush(COLOR_BTNFACE);

                const COLORREF background = dark
                    ? RGB(32, 32, 32)
                    : GetSysColor(COLOR_BTNFACE);

                SetTextColor(dc, contrastingTextColor(background));
                SetBkMode(dc, TRANSPARENT);

                return reinterpret_cast<LRESULT>(brush);
            }
            if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
                HDC dc = reinterpret_cast<HDC>(wp);

                const COLORREF back = dark
                    ? kDarkEditBack
                    : GetSysColor(COLOR_WINDOW);

                SetTextColor(dc, dark
                    ? kDarkEditText
                    : GetSysColor(COLOR_WINDOWTEXT));

                SetBkColor(dc, back);

                return reinterpret_cast<LRESULT>(
                    dark ? darkEditBrush() : GetSysColorBrush(COLOR_WINDOW)
                    );
            }
            if (wnd != m_wnd) return DefWindowProcW(wnd, msg, wp, lp);

            if (msg == WM_COMMAND) {
                const int id = LOWORD(wp);
                const int code = HIWORD(wp);
                if (id == ID_LIST && code == LBN_SELCHANGE) {
                    storeCurrent();
                    const int sel = (int)SendMessageW(m_list, LB_GETCURSEL, 0, 0);
                    if (sel >= 0) m_current = (size_t)sel;
                    loadCurrent();
                    runTest();
                    return 0;
                }
                if (id == ID_ADD && code == BN_CLICKED) { addSlot(); return 0; }
                if (id == ID_REMOVE && code == BN_CLICKED) { removeSlot(); return 0; }
                if (id == ID_EDITOR && code == BN_CLICKED) { openEditor(); return 0; }
                if (id == ID_EXPORT && code == BN_CLICKED) { exportSettings(); return 0; }
                if (id == ID_TABLEDIR && code == BN_CLICKED) { openTablesFolder(); return 0; }
                if (id == ID_TABLE && code == CBN_DROPDOWN) {
                    storeCurrent();
                    fillTableCombo();  // pick up files added since the page opened
                    return 0;
                }
                if (id == ID_TABLE && code == CBN_SELCHANGE) {
                    storeCurrent();
                    if (m_callback.is_valid()) m_callback->on_state_changed();
                    runTest();
                    return 0;
                }
                if (id == ID_IMPORT && code == BN_CLICKED) { importSettings(); return 0; }
                if (id == ID_LANG && code == CBN_SELCHANGE) {
                    const int sel = (int)SendMessageW(m_langCombo, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && (size_t)sel < m_langCodes.size()) m_langPending = m_langCodes[(size_t)sel];
                    if (m_callback.is_valid()) m_callback->on_state_changed();
                    return 0;
                }
                if ((id == ID_NAME || id == ID_SCRIPT) && code == EN_CHANGE && m_ready && !m_loading) {
                    if (id == ID_NAME) {
                        storeCurrent();
                        updateHint();
                        refreshList();
                    }
                    else {
                        // Preview shortly after the last keystroke so long scripts do not lag.
                        SetTimer(m_wnd, ID_PREVIEW_TIMER, 300, nullptr);
                    }
                    if (m_callback.is_valid()) m_callback->on_state_changed();
                    return 0;
                }
            }
            if (msg == WM_APP + 1 && m_ready && !m_loading) { runTest(); return 0; }
            if (msg == WM_TIMER && wp == ID_PREVIEW_TIMER) {
                KillTimer(m_wnd, ID_PREVIEW_TIMER);
                if (m_ready && !m_loading) runTest();
                return 0;
            }
            if (msg == WM_SIZE) layoutControls(LOWORD(lp), HIWORD(lp));
            if (msg == WM_DPICHANGED || msg == WM_DPICHANGED_AFTERPARENT) applyDpi();
            if (msg == WM_THEMECHANGED || msg == WM_SYSCOLORCHANGE || msg == WM_SETTINGCHANGE)
                InvalidateRect(m_wnd, nullptr, TRUE);
            if (msg == WM_DESTROY) {
                KillTimer(m_wnd, ID_PREVIEW_TIMER);
                m_wnd = nullptr;
            }
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        preferences_page_callback::ptr m_callback;
        HWND m_wnd;
        bool m_ready;
        bool m_loading = false;
        size_t m_current;
        HWND m_list, m_nameLabel, m_hint, m_field, m_scriptLabel, m_name, m_script, m_result, m_note, m_langLabel, m_langCombo, m_previewLabel, m_scriptButton, m_importButton, m_exportButton, m_tableLabel, m_tableCombo, m_tableFolder, m_addButton, m_removeButton;
        std::vector<std::string> m_names, m_scripts, m_tables;
        std::vector<std::uint32_t> m_ids;
        std::vector<SlotData> m_saved;  // as last saved
        std::uint32_t m_nextId = 1;
        std::vector<std::string> m_tableNames;
        std::string m_langPending;
        std::vector<std::string> m_langCodes;
        HFONT m_uiFont;
        HFONT m_monoFont;
        LOGFONTW m_baseLf{};
        UINT m_baseDpi = 96;
        HBRUSH m_brDark;
        fb2k::CCoreDarkModeHooks m_darkMode;
    };

    class tftags_preferences_page : public preferences_page_v3 {
    public:
        const char* get_name() override { return "TF Tags"; }
        GUID get_guid() override { return guid_tftags_preferences; }
        GUID get_parent_guid() override { return preferences_page::guid_tools; }
        preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override {
            return new service_impl_t<tftags_preferences_instance>(reinterpret_cast<HWND>(parent), callback);
        }
    };
    preferences_page_factory_t<tftags_preferences_page> g_tftags_preferences_page;

} // namespace