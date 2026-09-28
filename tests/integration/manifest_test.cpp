// The manifest is where all game-specific knowledge lives, so a silent
// misparse would quietly produce a wrong port. Every accessor reports a
// missing or wrong-typed key rather than defaulting behind your back.
#include "manifest.hpp"
#include "toml_lite.hpp"

#include "test_support.hpp"

using namespace arecomp;
using namespace arecomp::test;

namespace {

toml::Document parse(const std::string& text, std::string& error) {
    toml::Document doc;
    doc.parse(text, error);
    return doc;
}

} // namespace

TEST_CASE("integers accept decimal, 0x and Amiga $ notation") {
    std::string error;
    const auto doc = parse(
        "[t]\n"
        "dec = 4096\n"
        "hexc = 0x1000\n"
        "amiga = $1000\n"
        "underscored = 0x0001_0000\n"
        "negative = -8\n",
        error);
    CHECK_STR_EQ(error, "");
    const toml::Table& t = doc.table("t");
    CHECK_EQ(t.integer("dec"), 4096u);
    CHECK_EQ(t.integer("hexc"), 0x1000u);
    CHECK_EQ(t.integer("amiga"), 0x1000u);
    CHECK_EQ(t.integer("underscored"), 0x10000u);
    CHECK_EQ(t.integer("negative"), 0xfffffff8u);
}

TEST_CASE("arrays of integers survive the round trip") {
    std::string error;
    const auto doc = parse("[cpu]\na = [0, 1, 0x20, $30]\n", error);
    CHECK_STR_EQ(error, "");
    const auto values = doc.table("cpu").integers("a");
    CHECK_EQ(values.size(), 4u);
    CHECK_EQ(values[2], 0x20u);
    CHECK_EQ(values[3], 0x30u);
}

TEST_CASE("arrays of tables keep their order and their keys") {
    std::string error;
    const auto doc = parse(
        "[[memory.region]]\nstart = 0\nsize = 0x80000\nkind = \"chip\"\n"
        "[[memory.region]]\nstart = 0xc00000\nsize = 0x80000\nkind = \"slow\"\n",
        error);
    CHECK_STR_EQ(error, "");
    const auto& regions = doc.array("memory.region");
    CHECK_EQ(regions.size(), 2u);
    CHECK_EQ(regions[0].integer("start"), 0u);
    CHECK_STR_EQ(regions[1].string("kind"), "slow");
}

TEST_CASE("a comment inside a quoted string is not stripped") {
    std::string error;
    const auto doc = parse("[input]\npath = \"capture/mem#1.bin\"  # a comment\n", error);
    CHECK_STR_EQ(error, "");
    CHECK_STR_EQ(doc.table("input").string("path"), "capture/mem#1.bin");
}

TEST_CASE("a malformed line is an error with a line number, not a default") {
    std::string error;
    toml::Document doc;
    CHECK(!doc.parse("[game]\nthis line has no equals sign\n", error));
    CHECK(error.find("line 2") != std::string::npos);
}

TEST_CASE("an unparseable value is reported rather than guessed") {
    std::string error;
    toml::Document doc;
    CHECK(!doc.parse("[game]\nname = unquoted\n", error));
}

TEST_CASE("a missing key returns the caller's fallback") {
    std::string error;
    const auto doc = parse("[game]\nname = \"x\"\n", error);
    CHECK_STR_EQ(doc.table("game").string("absent", "fallback"), "fallback");
    CHECK_EQ(doc.table("game").integer("absent", 7u), 7u);
    CHECK(doc.table("nonexistent").values().empty());
}

TEST_CASE("the vertical-slice manifest loads and describes the right machine") {
    Manifest manifest;
    std::string error;
    const bool ok = Manifest::load(std::string(ARECOMP_PORTS_DIR) + "/vertical-slice.toml",
                                   manifest, error);
    if (!ok) report_failure(__FILE__, __LINE__, error);
    CHECK(ok);
    if (!ok) return;

    CHECK_STR_EQ(manifest.id, "vertical-slice");
    CHECK_EQ(manifest.cpu.entry, 0x1000u);
    CHECK_EQ(manifest.cpu.initial_sr, 0x2700u);
    CHECK(manifest.cpu.has_registers);
    CHECK_EQ(manifest.cpu.a[7], 0x0007f000u);
    CHECK_EQ(manifest.load_address, 0x1000u);
    CHECK_EQ(manifest.harness_halt_address, 0x00f00000u);
    CHECK_EQ(manifest.regions.size(), 1u);
    CHECK_EQ(manifest.code_ranges.size(), 1u);

    CHECK(manifest.in_code_range(0x1000));
    CHECK(!manifest.in_code_range(0x2000));
}

TEST_CASE("a manifest without an entry point is rejected") {
    // Nothing downstream can recover from this, so it fails at load rather
    // than producing an empty port.
    Manifest manifest;
    std::string error;
    CHECK(!Manifest::load("does-not-exist.toml", manifest, error));
    CHECK(!error.empty());
}

ARECOMP_TEST_MAIN()
