#include "manifest.hpp"

#include "toml_lite.hpp"

namespace arecomp {

namespace {

bool parse_input_mode(const std::string& text, InputMode& out, std::string& error) {
    if (text == "flat" || text.empty()) { out = InputMode::Flat; return true; }
    if (text == "snapshot") { out = InputMode::Snapshot; return true; }
    if (text == "hunk") { out = InputMode::Hunk; return true; }
    if (text == "adf") { out = InputMode::Adf; return true; }
    error = "unknown input type: " + text;
    return false;
}

bool is_absolute(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    return path.size() > 2 && path[1] == ':';
}

} // namespace

std::string Manifest::directory() const {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string Manifest::resolve(const std::string& relative) const {
    if (relative.empty() || is_absolute(relative)) return relative;
    return directory() + "/" + relative;
}

bool Manifest::in_code_range(u32 address) const {
    if (code_ranges.empty()) return true;   // unconstrained
    for (const CodeRangeSpec& range : code_ranges) {
        if (address >= range.start && address < range.end) return true;
    }
    return false;
}

bool Manifest::load(const std::string& path, Manifest& out, std::string& error) {
    toml::Document doc;
    if (!doc.parse_file(path, error)) return false;

    out.path = path;

    const toml::Table& game = doc.table("game");
    out.name = game.string("name", "unnamed");
    out.id = game.string("id", "port");
    out.machine = game.string("machine", "a500-pal");
    out.sha256 = game.string("sha256");

    const toml::Table& input = doc.table("input");
    if (!parse_input_mode(input.string("type", "flat"), out.input_mode, error))
        return false;
    // The spec's snapshot example names the key "memory"; a plain image uses
    // "path". Accept either rather than being pointlessly strict.
    out.input_path = input.string("path", input.string("memory"));
    if (out.input_path.empty()) {
        error = "[input] needs a path (or memory) key";
        return false;
    }
    out.load_address = input.integer("load_address", 0);
    out.disk_path = input.string("disk");
    out.chipset_state_path = input.string("chipset_state");
    if (out.disk_path.empty() && out.input_mode == InputMode::Adf)
        out.disk_path = out.input_path;

    out.video_standard = doc.table("video").string("standard", "pal");

    const toml::Table& cpu = doc.table("cpu");
    out.cpu.entry = cpu.integer("entry", cpu.integer("pc", 0));
    out.cpu.initial_sr = static_cast<u16>(cpu.integer("sr", cpu.integer("initial_sr", 0x2700)));
    const std::vector<u32> d = cpu.integers("d");
    const std::vector<u32> a = cpu.integers("a");
    if (!d.empty() || !a.empty()) {
        out.cpu.has_registers = true;
        for (std::size_t i = 0; i < d.size() && i < 8; ++i) out.cpu.d[i] = d[i];
        for (std::size_t i = 0; i < a.size() && i < 8; ++i) out.cpu.a[i] = a[i];
    }
    if (out.cpu.entry == 0 && out.input_mode != InputMode::Hunk) {
        error = "[cpu] entry is required (there is nowhere to start translating)";
        return false;
    }

    const toml::Table& opts = doc.table("recompiler");
    out.options.strict_address_errors = opts.boolean("strict_address_errors", true);
    out.options.development_interpreter_fallback =
        opts.boolean("development_interpreter_fallback", true);
    out.options.emit_instruction_comments = opts.boolean("emit_instruction_comments", true);
    out.options.emit_cycle_accounting = opts.boolean("emit_cycle_accounting", true);
    out.options.blocks_per_file = opts.integer("blocks_per_file", 256);
    if (out.options.blocks_per_file == 0) out.options.blocks_per_file = 256;

    for (const toml::Table& t : doc.array("memory.region")) {
        MemoryRegionSpec region;
        region.start = t.integer("start", 0);
        region.size = t.integer("size", 0);
        region.kind = t.string("kind", "chip");
        region.permissions = t.string("permissions", "rwx");
        if (region.size == 0) {
            error = "[[memory.region]] with a zero size";
            return false;
        }
        out.regions.push_back(region);
    }

    for (const toml::Table& t : doc.array("adf.load")) {
        AdfLoadSpec load;
        load.offset = t.integer("offset", 0);
        load.length = t.integer("length", 0);
        load.address = t.integer("address", 0);
        load.executable = t.boolean("executable", true);
        if (load.length == 0) {
            error = "[[adf.load]] with a zero length";
            return false;
        }
        out.adf_loads.push_back(load);
    }

    for (const toml::Table& t : doc.array("code.range")) {
        CodeRangeSpec range;
        range.start = t.integer("start", 0);
        range.end = t.integer("end", 0);
        if (range.end <= range.start) {
            error = "[[code.range]] end must be greater than start";
            return false;
        }
        out.code_ranges.push_back(range);
    }

    for (const toml::Table& t : doc.array("indirect_target")) {
        IndirectTargetSpec spec;
        spec.site = t.integer("site", 0);
        spec.targets = t.integers("targets");
        out.indirect_targets.push_back(std::move(spec));
    }

    for (const toml::Table& t : doc.array("patch.instruction")) {
        PatchSpec patch;
        patch.address = t.integer("address", 0);
        patch.replacement = t.string("replacement", "nop");
        if (patch.replacement != "nop") {
            error = "unsupported instruction patch replacement: " + patch.replacement;
            return false;
        }
        out.patches.push_back(std::move(patch));
    }

    for (const toml::Table& t : doc.array("hook")) {
        HookSpec hook;
        hook.address = t.integer("address", 0);
        hook.name = t.string("name");
        hook.mode = t.string("mode", "before");
        if (hook.name.empty()) {
            error = "[[hook]] needs a name";
            return false;
        }
        if (hook.mode != "before" && hook.mode != "after" && hook.mode != "replace") {
            error = "unknown hook mode: " + hook.mode;
            return false;
        }
        out.hooks.push_back(std::move(hook));
    }

    out.extra_entry_points = doc.table("code").integers("entry_points");
    out.harness_halt_address = doc.table("harness").integer("halt_address", 0);
    out.enable_chipset = doc.table("machine").boolean("chipset", true);

    const toml::Table& cap = doc.table("capture");
    if (doc.has_table("capture")) {
        out.capture.enabled = cap.boolean("enabled", true);
        out.capture.trigger = cap.integer("trigger", 0);
        out.capture.start = cap.integer("start", 0);
        out.capture.size = cap.integer("size", 0x80000);
        out.capture.halt = cap.boolean("halt", true);
        out.capture.path = cap.string("path", "capture.bin");
        if (out.capture.enabled && out.capture.trigger == 0) {
            error = "[capture] needs a trigger address";
            return false;
        }
    }

    const toml::Table& ws = doc.table("widescreen");
    out.widescreen.enabled = ws.boolean("enabled", false);
    out.widescreen.extra_left = ws.integer("extra_left", 0);
    out.widescreen.extra_right = ws.integer("extra_right", 0);
    out.widescreen.compensate_modulo = ws.boolean("compensate_modulo", true);

    if (out.regions.empty()) {
        // A sensible A500 default so simple manifests stay short.
        out.regions.push_back({0x000000, 0x080000, "chip", "rwx"});
    }
    return true;
}

} // namespace arecomp
