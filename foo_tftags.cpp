// foo_tftags - publishes long titleformat scripts as global tag fields.
//
// Twenty slots. Each slot has a tag name and a titleformat script. The result of the script is
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
#include <functional>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
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

DECLARE_COMPONENT_VERSION("TF Tags", "0.1.1",
    "Publishes long titleformat scripts as global tag fields (%tf_<name>%).");
VALIDATE_COMPONENT_FILENAME("foo_tftags.dll");

namespace {

    constexpr size_t kSlots = 20;
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
        cfg_string name;
        cfg_string script;
        SlotConfig(unsigned index, const char* defaultName, const char* defaultScript)
            : nameGuid(makeGuid(0x6f1c4a00u + index, 0x01)),
            scriptGuid(makeGuid(0x6f1c4b00u + index, 0x02)),
            name(nameGuid, defaultName),
            script(scriptGuid, defaultScript) {
        }
    };

    SlotConfig g_slots[kSlots] = {
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

    // Thread-safe copy of the scripts. Other threads (panels, search, background rendering)
    // evaluate slots while the main thread applies preferences, so they never read the
    // cfg_string objects directly.
    struct ScriptStore {
        std::mutex m;
        std::string scripts[kSlots];
        bool loaded = false;
    };

    ScriptStore& scriptStore() {
        static ScriptStore* store = new ScriptStore;
        return *store;
    }

    std::atomic<unsigned> g_scriptVersion{ 0 };

    std::string getScript(size_t slot) {
        ScriptStore& st = scriptStore();
        std::lock_guard<std::mutex> lock(st.m);
        if (!st.loaded) {
            for (size_t i = 0; i < kSlots; ++i) {
                const char* v = g_slots[i].script.get();
                st.scripts[i] = v ? v : "";
            }
            st.loaded = true;
        }
        return st.scripts[slot];
    }

    // Main thread only: stores the scripts in the config and in the thread-safe copy.
    void setScripts(const std::string* scripts) {
        ScriptStore& st = scriptStore();
        std::lock_guard<std::mutex> lock(st.m);
        for (size_t i = 0; i < kSlots; ++i) {
            g_slots[i].script = scripts[i].c_str();
            st.scripts[i] = scripts[i];
        }
        st.loaded = true;
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

    // Field name for a slot, e.g. "tf_albumartist". Empty or duplicate names get a slot number.
    std::string fieldNameFor(size_t index, const std::string* names /* kSlots entries, or null = saved */) {
        auto nameOf = [&](size_t i) {
            return cleanName(names ? names[i] : std::string(g_slots[i].name.get() ? g_slots[i].name.get() : ""));
            };
        std::string n = nameOf(index);
        if (n.empty()) n = "slot" + std::to_string(index + 1);
        for (size_t i = 0; i < index; ++i) {
            std::string other = nameOf(i);
            if (other.empty()) other = "slot" + std::to_string(i + 1);
            if (other == n) { n += "_" + std::to_string(index + 1); break; }
        }
        return std::string(kFieldPrefix) + n;
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
    std::string findCycle(const std::string* names, const std::string* scripts) {
        std::string field[kSlots];
        std::vector<size_t> edges[kSlots];
        for (size_t i = 0; i < kSlots; ++i) field[i] = fieldNameFor(i, names);

        for (size_t i = 0; i < kSlots; ++i) {
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
                for (size_t j = 0; j < kSlots; ++j)
                    if (token == field[j] && std::find(edges[i].begin(), edges[i].end(), j) == edges[i].end())
                        edges[i].push_back(j);
                p = q;
            }
        }

        for (size_t start = 0; start < kSlots; ++start) {
            size_t parent[kSlots] = {};
            bool seen[kSlots] = {};
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
                if (!seen[j]) { seen[j] = true; parent[j] = start; queue.push_back(j); }
            }
            for (size_t head = 0; head < queue.size(); ++head) {
                const size_t u = queue[head];
                for (size_t v : edges[u]) {
                    if (v == start) return describe(u);
                    if (!seen[v]) { seen[v] = true; parent[v] = u; queue.push_back(v); }
                }
            }
        }
        return {};
    }

    // ---------------------------------------------------------------- dynamic (ICY) metadata
    // Same scheme as foo_countryflags: a playback callback remembers the playing track and
    // whether its stream has delivered live metadata. While it has, the track is formatted by
    // foobar2000's own playback formatting, which applies the live values to every field
    // (%artist%, %title%, ...), so no field needs to be listed here and no hook is involved.

    struct DynamicInfo {
        std::mutex m;
        metadb_handle_ptr track;
        bool live = false;
    };

    DynamicInfo& dynamicInfo() {
        static DynamicInfo* d = new DynamicInfo;
        return *d;
    }

    // The track a preview is evaluated for. A selected track always wins, even while another
    // track is playing: the active playlist selection first, then the selection of any other
    // panel (e.g. the library), then the focused playlist row. The playing track is only used
    // when none of these exist.
    metadb_handle_ptr pickPreviewTrack() {
        metadb_handle_ptr playing;
        playback_control::get()->get_now_playing(playing);
        metadb_handle_list sel;
        playlist_manager::get()->activeplaylist_get_selected_items(sel);
        if (sel.get_count() == 0) ui_selection_manager::get()->get_selection(sel);
        if (sel.get_count() > 0) return sel[0];
        metadb_handle_ptr focused;
        if (playlist_manager::get()->activeplaylist_get_focus_item_handle(focused) && focused.is_valid()) return focused;
        return playing;
    }

    // Preview windows also poll the picked track at this interval, so a changed selection or
    // focus refreshes them (those changes are not announced to plugins).
    constexpr UINT kTrackWatchMs = 300;

    // Preview windows (the preferences page and the script editor) register here and are told to
    // refresh when the playing track, or its live stream metadata, changes.
    constexpr UINT WM_TFTAGS_REFRESH_PREVIEW = WM_APP + 2;

    struct PreviewTargets {
        std::mutex m;
        std::vector<HWND> wnds;
    };

    PreviewTargets& previewTargets() {
        static PreviewTargets* t = new PreviewTargets;
        return *t;
    }

    void registerPreviewWindow(HWND wnd) {
        PreviewTargets& t = previewTargets();
        std::lock_guard<std::mutex> lock(t.m);
        if (std::find(t.wnds.begin(), t.wnds.end(), wnd) == t.wnds.end()) t.wnds.push_back(wnd);
    }

    void unregisterPreviewWindow(HWND wnd) {
        PreviewTargets& t = previewTargets();
        std::lock_guard<std::mutex> lock(t.m);
        t.wnds.erase(std::remove(t.wnds.begin(), t.wnds.end(), wnd), t.wnds.end());
    }

    void notifyPreviewWindows() {
        PreviewTargets& t = previewTargets();
        std::lock_guard<std::mutex> lock(t.m);
        for (HWND wnd : t.wnds) PostMessageW(wnd, WM_TFTAGS_REFRESH_PREVIEW, 0, 0);
    }

    class dynamic_info_callback : public play_callback_static {
    public:
        unsigned get_flags() override {
            return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_dynamic_info_track;
        }
        void on_playback_new_track(metadb_handle_ptr p_track) override {
            DynamicInfo& d = dynamicInfo();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.track = p_track;
                d.live = false;
            }
            notifyPreviewWindows();
        }
        void on_playback_stop(play_control::t_stop_reason) override {
            DynamicInfo& d = dynamicInfo();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.track.release();
                d.live = false;
            }
            notifyPreviewWindows();
        }
        void on_playback_dynamic_info_track(const file_info& p_info) override {
            DynamicInfo& d = dynamicInfo();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.live = p_info.meta_get_count() > 0;
            }
            notifyPreviewWindows();
        }
        void on_playback_starting(play_control::t_track_command, bool) override {}
        void on_playback_seek(double) override {}
        void on_playback_pause(bool) override {}
        void on_playback_edited(metadb_handle_ptr) override {}
        void on_playback_dynamic_info(const file_info&) override {}
        void on_playback_time(double) override {}
        void on_volume_change(float) override {}
    };
    play_callback_static_factory_t<dynamic_info_callback> g_dynamic_info_callback;

    // True when h is the playing track and its stream has delivered live metadata.
    bool hasLiveInfo(metadb_handle* h) {
        DynamicInfo& d = dynamicInfo();
        std::lock_guard<std::mutex> lock(d.m);
        return d.live && d.track.is_valid() && d.track.get_ptr() == h;
    }

    // Formats a track; the playing one with live stream metadata goes through playback
    // formatting so every field reflects the live values. playback_format_title is only used on
    // the main thread (the SDK does not promise it is thread-safe, and it formats whatever is
    // playing at that moment); other threads get the track's normal tags.
    void formatTrack(metadb_handle* h, pfc::string_base& out, const titleformat_object::ptr& script) {
        if (core_api::is_main_thread() && hasLiveInfo(h) &&
            playback_control::get()->playback_format_title(nullptr, out, script, nullptr, playback_control::display_level_all))
            return;
        h->format_title(nullptr, out, script, nullptr);
    }

    // ---------------------------------------------------------------- evaluation

    // Cache compiled titleformat objects only. The script version is compared on every
    // request so preference edits take effect immediately; evaluated values are never cached.
    struct ScriptCache {
        std::mutex m;
        unsigned version = 0;
        titleformat_object::ptr obj;
        bool init = false;
    };

    ScriptCache& scriptCache(size_t slot) {
        static ScriptCache* caches[kSlots] = {
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache,
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache,
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache,
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache
        };
        return *caches[slot];
    }

    // A script that does not compile gives no object, so its field is empty.
    titleformat_object::ptr compileScript(size_t slot) {
        ScriptCache& c = scriptCache(slot);
        const unsigned version = g_scriptVersion.load();
        {
            std::lock_guard<std::mutex> lock(c.m);
            if (c.init && c.version == version) return c.obj;
        }
        const std::string flat = flattenScript(getScript(slot));
        titleformat_object::ptr obj;
        if (!flat.empty() && !titleformat_compiler::get()->compile(obj, flat.c_str())) obj.release();
        std::lock_guard<std::mutex> lock(c.m);
        c.obj = obj;
        c.version = version;
        c.init = true;
        return obj;
    }

    class script_cache_cleanup : public initquit {
    public:
        void on_quit() override {
            {
                DynamicInfo& d = dynamicInfo();
                std::lock_guard<std::mutex> lock(d.m);
                d.track.release();
                d.live = false;
            }
            for (size_t i = 0; i < kSlots; ++i) {
                ScriptCache& c = scriptCache(i);
                std::lock_guard<std::mutex> lock(c.m);
                c.obj.release();
                c.init = false;
            }
        }
    };
    initquit_factory_t<script_cache_cleanup> g_script_cache_cleanup;

    // Evaluates one slot for one track. A slot that is already being evaluated on this thread
    // (a reference cycle) returns an empty string.
    std::string evalSlot(metadb_handle* h, size_t slot) {
        thread_local unsigned inProgress = 0;
        const unsigned bit = 1u << slot;
        if (inProgress & bit) return {};

        std::string value;
        titleformat_object::ptr script = compileScript(slot);
        if (script.is_valid()) {
            struct Guard { unsigned& flags; unsigned b; ~Guard() { flags &= ~b; } } guard{ inProgress, bit };
            inProgress |= bit;
            pfc::string8 result;
            formatTrack(h, result, script);
            value = result.c_str();
        }
        return value;
    }

    class tf_fields : public metadb_display_field_provider {
    public:
        t_uint32 get_field_count() override { return (t_uint32)kSlots; }

        void get_field_name(t_uint32 index, pfc::string_base& out) override {
            if (index >= kSlots) { out.reset(); return; }
            out.set_string(fieldNameFor(index, nullptr).c_str());
        }

        bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override {
            if (index >= kSlots) return false;
            const std::string value = evalSlot(handle, index);
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
        { "note_3", "Changing a tag name asks to restart foobar2000 when you press Apply; script changes apply at once." },
        { "cycle_apply", "Circular reference between slots: {0}\n\nNothing was applied. Remove the reference and press Apply again." },
        { "cycle_test", "Circular reference between slots: {0}" },
        { "broken_scripts", "These scripts do not compile, so their fields will be empty:{0}\n\nApply anyway?" },
        { "restart_prompt", "A tag name was changed. foobar2000 reads field names at startup, so the new names only take effect after a restart.\n\nRestart foobar2000 now?" },
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
        { "import_confirm", "Replace the tag names and scripts of the slots in this file with the imported settings?\n\nThey are applied when you press Apply." },
        { "import_invalid", "This is not a TF Tags settings file." },
        { "import_failed", "The settings file could not be read." },
        { "export_failed", "The settings file could not be written." },
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

    HFONT createMonoFont(int lfHeight) {
        return CreateFontW(lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
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
                if (dark) SetTextColor(dc, RGB(240, 240, 240));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
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
        static constexpr UINT_PTR TIMER_WATCH = 2;

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

            m_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"foo_tftags_editor", m_title.c_str(),
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
                x, y, w, h, m_owner, nullptr, core_api::get_my_instance(), this);
            if (!m_wnd) {
                DeleteObject(m_font);
                DeleteObject(m_mono);
                return false;
            }

            registerPreviewWindow(m_wnd);
            m_lastTrack = pickPreviewTrack();
            SetTimer(m_wnd, TIMER_WATCH, kTrackWatchMs, nullptr);
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
            KillTimer(m_wnd, TIMER_WATCH);
            unregisterPreviewWindow(m_wnd);
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
                if (dark) SetTextColor(dc, RGB(240, 240, 240));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
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
                if (wp == TIMER_WATCH) {
                    const metadb_handle_ptr track = pickPreviewTrack();
                    if (track.get_ptr() != m_lastTrack.get_ptr()) {
                        m_lastTrack = track;
                        updatePreview();
                    }
                    return 0;
                }
                break;
            case WM_TFTAGS_REFRESH_PREVIEW:
                updatePreview();
                return 0;
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
        metadb_handle_ptr m_lastTrack;
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
    constexpr int ID_WATCH_TIMER = 2005;    // follows the selected track
    constexpr int ID_RESULT = 2005;
    constexpr int ID_LANG = 2006;
    constexpr int ID_EDITOR = 2007;
    constexpr int ID_IMPORT = 2008;
    constexpr int ID_EXPORT = 2009;

    class tftags_preferences_instance : public preferences_page_instance {
    public:
        tftags_preferences_instance(HWND parent, preferences_page_callback::ptr callback)
            : m_callback(callback), m_wnd(nullptr), m_ready(false), m_current(0),
            m_list(nullptr), m_nameLabel(nullptr), m_hint(nullptr), m_field(nullptr), m_scriptLabel(nullptr),
            m_name(nullptr), m_script(nullptr), m_result(nullptr), m_note(nullptr), m_langLabel(nullptr), m_langCombo(nullptr), m_previewLabel(nullptr), m_scriptButton(nullptr), m_importButton(nullptr), m_exportButton(nullptr),
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
            registerPreviewWindow(m_wnd);
            m_lastTrack = pickPreviewTrack();
            SetTimer(m_wnd, ID_WATCH_TIMER, kTrackWatchMs, nullptr);

            for (size_t i = 0; i < kSlots; ++i) {
                m_names[i] = g_slots[i].name.get() ? g_slots[i].name.get() : "";
                m_scripts[i] = g_slots[i].script.get() ? g_slots[i].script.get() : "";
            }

            m_baseDpi = baseUiFont(m_wnd, m_baseLf);
            rebuildFonts();

            m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), hInstance, nullptr);
            setFont(m_list, uiFont());

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

            for (size_t i = 0; i < kSlots; ++i) SendMessageW(m_list, LB_ADDSTRING, 0, 0);
            refreshList();
            SendMessageW(m_list, LB_SETCURSEL, 0, 0);
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
            std::string broken;
            for (size_t i = 0; i < kSlots; ++i) {
                const std::string flat = flattenScript(m_scripts[i]);
                titleformat_object::ptr obj;
                if (!flat.empty() && !titleformat_compiler::get()->compile(obj, flat.c_str()))
                    broken += "\n    %" + fieldNameFor(i, m_names) + "%";
            }
            if (!broken.empty()) {
                const std::string msg = trf("broken_scripts", broken);
                if (themed_message::show(GetAncestor(m_wnd, GA_ROOT), "TF Tags", msg, true, true, IDI_WARNING) != IDYES) return;
            }

            std::string fieldsBefore[kSlots];
            for (size_t i = 0; i < kSlots; ++i) fieldsBefore[i] = fieldNameFor(i, nullptr);
            bool scriptsChanged = false;
            for (size_t i = 0; i < kSlots; ++i) {
                const char* old = g_slots[i].script.get();
                if (m_scripts[i] != (old ? old : "")) scriptsChanged = true;
            }
            for (size_t i = 0; i < kSlots; ++i) {
                g_slots[i].name = cleanName(m_names[i]).c_str();
                m_names[i] = cleanName(m_names[i]);
            }
            setScripts(m_scripts);
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

            // Field names are only read at startup, so a changed effective field name
            // needs a restart. Settings are written on exit, so the new names survive it.
            bool renamed = false;
            for (size_t i = 0; i < kSlots; ++i)
                if (fieldNameFor(i, nullptr) != fieldsBefore[i]) { renamed = true; break; }
            if (renamed) {
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
            for (size_t i = 0; i < kSlots; ++i) {
                std::string defaultName = std::to_string(i + 1);
                if (defaultName.size() < 2) defaultName.insert(defaultName.begin(), '0');
                m_names[i] = defaultName;
                m_scripts[i].clear();
            }
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
            for (HWND c : { m_list, m_nameLabel, m_name, m_hint, m_field, m_scriptLabel, m_result, m_note, m_langLabel, m_langCombo, m_previewLabel, m_scriptButton, m_importButton, m_exportButton })
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
            if (m_current >= kSlots) return;
            m_names[m_current] = readWindow(m_name);
            m_scripts[m_current] = readWindow(m_script);
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
            updateHint();
        }

        void refreshList() {
            const int sel = (int)SendMessageW(m_list, LB_GETCURSEL, 0, 0);
            SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
            for (size_t i = 0; i < kSlots; ++i) {
                std::string slotNumber = std::to_string(i + 1);
                if (slotNumber.size() < 2) slotNumber.insert(slotNumber.begin(), '0');
                const std::string text = slotNumber + "   %" + fieldNameFor(i, m_names) + "%";
                pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
                SendMessageW(m_list, LB_DELETESTRING, i, 0);
                SendMessageW(m_list, LB_INSERTSTRING, i, reinterpret_cast<LPARAM>(wide.get_ptr()));
            }
            SendMessageW(m_list, LB_SETCURSEL, sel < 0 ? 0 : sel, 0);
            SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(m_list, nullptr, TRUE);
        }

        bool hasChanges() {
            storeCurrent();
            if (m_langPending != savedLanguage()) return true;
            for (size_t i = 0; i < kSlots; ++i) {
                const char* n = g_slots[i].name.get();
                const char* s = g_slots[i].script.get();
                if (cleanName(m_names[i]) != cleanName(n ? n : "")) return true;
                if (m_scripts[i] != (s ? s : "")) return true;
            }
            return false;
        }

        // Evaluates a script for the selected track and returns the text for a preview box.
        // `script` replaces the current slot's script (other slots use their saved scripts).
        std::string previewFor(const std::string& script) {
            std::string scripts[kSlots];
            for (size_t i = 0; i < kSlots; ++i) scripts[i] = m_scripts[i];
            scripts[m_current] = script;
            const std::string cycle = findCycle(m_names, scripts);
            if (!cycle.empty()) return trf("cycle_test", cycle);

            const metadb_handle_ptr track = pickPreviewTrack();
            if (track.is_empty()) return tr("test_no_track");

            const std::string flat = flattenScript(script);
            titleformat_object::ptr obj;
            if (flat.empty() || !titleformat_compiler::get()->compile(obj, flat.c_str())) return tr("test_empty_script");
            pfc::string8 out;
            formatTrack(track.get_ptr(), out, obj);
            return out.is_empty() ? tr("test_empty_result") : std::string(out.c_str());
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
            for (size_t i = 0; i < kSlots; ++i) {
                data += slotKey("name", i) + "=" + escapeValue(cleanName(m_names[i])) + "\n";
                data += slotKey("script", i) + "=" + escapeValue(m_scripts[i]) + "\n";
            }
            FILE* f = nullptr;
            const bool ok = _wfopen_s(&f, path.c_str(), L"wb") == 0 && f
                && fwrite(data.data(), 1, data.size(), f) == data.size();
            if (f) fclose(f);
            if (!ok) themed_message::show(GetAncestor(m_wnd, GA_ROOT), "TF Tags", tr("export_failed"), false, false, IDI_WARNING);
        }

        // Loads names and scripts into the page; they take effect when Apply is pressed.
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
            if (themed_message::show(owner, "TF Tags", tr("import_confirm"), true, false, IDI_QUESTION) != IDYES) return;

            storeCurrent();
            for (size_t i = 0; i < kSlots; ++i) {
                const auto name = table.find(slotKey("name", i));
                const auto script = table.find(slotKey("script", i));
                if (name != table.end()) m_names[i] = cleanName(name->second);
                if (script != table.end()) m_scripts[i] = withCrLf(script->second);
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
            place(m_list, margin, margin, listW, (std::max)(S(60), contentBottom - margin));

            int y = margin;
            place(m_nameLabel, rightX, y, rightW, labelH); y += labelH + S(2);
            place(m_name, rightX, y, (std::min)(rightW, S(260)), rowH);
            y += rowH + S(2);
            // The label and the selectable field share one row; the field shrinks on narrow pages.
            const int hintW = textWidth(m_hint);
            place(m_hint, rightX, y + (rowH - labelH) / 2, hintW, labelH);
            place(m_field, rightX + hintW + gap, y, (std::max)(S(80), (std::min)(rightW - hintW - gap, S(260))), rowH);
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
                HDC dc = (HDC)wp;
                if (dark) SetTextColor(dc, RGB(240, 240, 240));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
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
                if (id == ID_EDITOR && code == BN_CLICKED) { openEditor(); return 0; }
                if (id == ID_EXPORT && code == BN_CLICKED) { exportSettings(); return 0; }
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
            if (msg == WM_TFTAGS_REFRESH_PREVIEW) {
                if (m_ready && !m_loading) runTest();
                return 0;
            }
            if (msg == WM_TIMER && wp == ID_WATCH_TIMER) {
                if (m_ready && !m_loading) {
                    const metadb_handle_ptr track = pickPreviewTrack();
                    if (track.get_ptr() != m_lastTrack.get_ptr()) {
                        m_lastTrack = track;
                        runTest();
                    }
                }
                return 0;
            }
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
                KillTimer(m_wnd, ID_WATCH_TIMER);
                unregisterPreviewWindow(m_wnd);
                m_wnd = nullptr;
            }
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        preferences_page_callback::ptr m_callback;
        HWND m_wnd;
        bool m_ready;
        bool m_loading = false;
        size_t m_current;
        HWND m_list, m_nameLabel, m_hint, m_field, m_scriptLabel, m_name, m_script, m_result, m_note, m_langLabel, m_langCombo, m_previewLabel, m_scriptButton, m_importButton, m_exportButton;
        metadb_handle_ptr m_lastTrack;
        std::string m_langPending;
        std::vector<std::string> m_langCodes;
        HFONT m_uiFont;
        HFONT m_monoFont;
        LOGFONTW m_baseLf{};
        UINT m_baseDpi = 96;
        HBRUSH m_brDark;
        std::string m_names[kSlots];
        std::string m_scripts[kSlots];
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