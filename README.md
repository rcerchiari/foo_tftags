# foo_tftags

A foobar2000 component that publishes long titleformat scripts as **global tag fields**. Write a script once (for example your whole BIO_ALBUMARTIST or BIO_ARTIST code) and use `%tf_albumartist%` everywhere that evaluates title formatting: playlist views, CPV, PSS/ELP, JS3 scripts, ESlyrics, Last.fm scribbler, and so on.

## Slots

There are twenty slots. Each has a **tag name** and a **titleformat script**. Open **Preferences > Tools > TF Tags**.

Slot *n* starts with the tag name `01` ... `20` (zero-padded, so `%tf_01%` ... `%tf_20%`) and an empty script.

The field name is always `tf_` plus the tag name, so slot 1 is `%tf_albumartist%`. Tag names are lowercased and only letters, digits and `_` are kept. Empty or duplicate names get a slot number (`tf_slot4`, `tf_artist_2`). An empty script (or a script that returns nothing) makes the field empty, so `$if(%tf_artist%,...)` works.

## Writing scripts

- Scripts can span several lines. Before compiling, leading and trailing whitespace of every line is removed, `//` comments (a whole line or the end of a line; text inside 'quotes' and `://` are left alone) are removed, and the lines are joined. Do not rely on line breaks inside quoted text.
- A slot may use other slots: `%tf_artist%` inside slot 3 works. Circular references (a slot using its own field, directly or through other slots) are detected when you press **Apply**: nothing is applied and a message shows the cycle, e.g. `tf_a -> tf_b -> tf_a`. The **Preview** reports it too. As a safety net the evaluator still returns nothing for a cycle instead of looping.
- **Preview:** the box at the bottom shows the result of the script in the editor (saved or not) for the selected track, even while another track is playing: the active playlist's selection first, then the selection of another panel (e.g. the library), then the focused playlist row. Only if none of these exist does it use the playing track. It updates when you click a slot, the tag name or the script, when you move through the slot list with the arrow keys, and about 0.3 seconds after you stop typing in the script. It also follows the track: it refreshes when you select another track or move the playlist focus (checked about every 0.3 seconds), when playback moves to a new track or stops, and when a stream delivers new metadata. Other slots used inside it (`%tf_artist%`) are evaluated with their saved scripts.
- **"..." button** (right of the script label) opens a larger, resizable editor for long scripts, with its own live preview that updates about 0.3 seconds after you stop typing and whenever the selected or playing track changes. Tab inserts a tab character and Ctrl+A selects all. **OK** copies the text back to the script box (still not applied until you press **Apply**); **Cancel**, Esc or closing the window asks before discarding changes.
- **Apply** checks that every script compiles. If one does not, you can still apply (that slot's field is then empty) or go back and fix it. After Apply, the active playlist is refreshed so changed scripts show up.

## Import and export

**Export...** (bottom right of the page) saves the tag names and scripts of all 20 slots, including changes you have not applied yet, to a UTF-8 text file (`.tftags`, one `name.NN=` and `script.NN=` line per slot, line breaks written as `\n`). **Import...** reads such a file into the page after a confirmation: slots in the file replace the page's names and scripts, other slots are left alone, and nothing takes effect until you press **Apply** (renaming a tag still asks to restart). The language setting is not part of the file.

## Restart and caching

- foobar2000 reads field names at startup, so **changing a tag name makes foobar2000 ask whether to restart when you press Apply** (answer No to restart later; the new name only takes effect after a restart). Script-only changes apply immediately without a restart.
- Only the compiled scripts are cached. Values are evaluated fresh on every request.


## Translations

The texts of the preferences page and its dialogs come from plain UTF-8 files in a `lang` folder next to the component DLL (inside the `.fb2k-component` archive: `lang\zh-CN.lang` beside `foo_tftags.dll`). The language is chosen from the Windows display language unless you pick one in the **Language** drop-down at the bottom of the page (it lists every `.lang` file by its `language_name`; the change applies when you press **Apply** and updates the page at once): `zh-CN.lang` (Simplified Chinese) is included, and Traditional Chinese regions (zh-TW, zh-HK, zh-MO) look for `zh-TW.lang` first, then fall back to `zh-CN.lang`. Any other language uses `<code>.lang` (e.g. `de-DE.lang`, then `de.lang`) and English when no file exists. Missing keys also fall back to English.

To add a language, copy `lang\en.lang`, rename it, set `language_name` (shown in the drop-down) and translate the text after each `=`. `\n` is a line break, `{0}` and `{1}` are placeholders, and lines starting with `#` are comments. Field names (`%tf_...%`), titleformat syntax and the component name are not translated.

## Includes

- Dynamic (stream) metadata


## Build

Visual Studio 2022, C++17, foobar2000 SDK. Link `foobar2000_SDK`, `foobar2000_component_client` and `pfc`, with the same project settings as foo_countryflags. The file must stay pure ASCII.

## License

MIT License


### Screenshot

![Alt text](/screenshot.png?raw=true "Preferences")