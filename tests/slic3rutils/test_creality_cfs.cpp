#include <catch2/catch_all.hpp>

#include "slic3r/Utils/CrealityCFS.hpp"

using namespace Slic3r::CrealityCFS;

TEST_CASE("CFS colour normalisation", "[CFS]")
{
    // 7-hex printer form "#0RRGGBB" drops the leading zero nibble.
    REQUIRE(normalize_color("#0f4e076") == "#F4E076");
    REQUIRE(normalize_color("#04b697d") == "#4B697D");
    // Already 6-hex passes through, upper-cased.
    REQUIRE(normalize_color("#ff8800") == "#FF8800");
    // 8-hex (with alpha/leading byte) keeps the last 6, prefixed with '#'.
    REQUIRE(normalize_color("AABBCCDD") == "#BBCCDD");
    // More than 8 hex digits: keep the trailing 6.
    REQUIRE(normalize_color("11223344556677") == "#556677");
    // Unparseable input yields an empty string.
    REQUIRE(normalize_color("not-a-colour").empty());
    REQUIRE(normalize_color("").empty());
}

TEST_CASE("CFS printer colour round-trips", "[CFS]")
{
    const std::string encoded = encode_printer_color("#FF0000");
    REQUIRE(encoded == "#0FF0000");
    REQUIRE(normalize_color(encoded) == "#FF0000");

    REQUIRE(encode_printer_color("invalid").empty());
}

TEST_CASE("CFS material type normalisation", "[CFS]")
{
    REQUIRE(normalize_material_type("  pla ") == "PLA");
    REQUIRE(normalize_material_type("petg") == "PETG");
}

TEST_CASE("CFS preset config keys and defaults", "[CFS]")
{
    REQUIRE(material_type_config_key("PLA") == "cfs_preset_map_pla");
    REQUIRE(material_type_config_key("PETG") == "cfs_preset_map_petg");
    // Non-alphanumeric characters become underscores, rest lower-cased.
    REQUIRE(material_type_config_key("PA-CF") == "cfs_preset_map_pa_cf");

    // No built-in default mapping: every type is unmapped until the user
    // configures it, so auto-apply never targets a non-existent preset.
    REQUIRE(default_preset_mapping("PLA").empty());
    REQUIRE(default_preset_mapping("PETG").empty());
    REQUIRE(default_preset_mapping("ABS").empty());

    const auto types = supported_material_types();
    REQUIRE_FALSE(types.empty());
    REQUIRE(std::find(types.begin(), types.end(), "PLA") != types.end());
    REQUIRE(std::find(types.begin(), types.end(), "PETG") != types.end());
}

TEST_CASE("CFS material preset definitions", "[CFS]")
{
    const auto* pla = find_material_preset_definition("PLA");
    REQUIRE(pla != nullptr);
    REQUIRE(std::string(pla->name) == "Generic PLA");

    REQUIRE(find_material_preset_definition("NOT-A-TYPE") == nullptr);
}

TEST_CASE("CFS boxsInfo parsing", "[CFS]")
{
    nlohmann::json box;
    box["type"]      = 0;
    box["id"]        = 1;
    box["materials"] = nlohmann::json::array();
    box["materials"].push_back({{"id", 0}, {"color", "#0FF0000"}, {"type", "pla"},
                                {"rfid", "00001"}, {"minTemp", 190}, {"maxTemp", 240}, {"pressure", 0.04}});
    box["materials"].push_back({{"id", 1}, {"color", "#000FF00"}, {"type", "petg"}});
    box["materials"].push_back({{"id", 2}, {"color", "#00000FF"}, {"type", "abs"}});
    box["materials"].push_back({{"id", 3}, {"color", "#0FFFFFF"}, {"type", "tpu"}});

    nlohmann::json root;
    root["boxsInfo"] = nlohmann::json::array();
    root["boxsInfo"].push_back(box);

    std::vector<SlotMaterial> slots(kSlotCount);
    REQUIRE(extract_materials_from_json(root, slots));
    REQUIRE(slots.size() == kSlotCount);

    REQUIRE(slots[0].material_id == 0);
    REQUIRE(slots[0].box_id == 1);
    REQUIRE(slots[0].color == "#FF0000");
    REQUIRE(slots[0].type == "PLA");
    REQUIRE(slots[0].rfid == "00001");

    REQUIRE(slots[1].color == "#00FF00");
    REQUIRE(slots[1].type == "PETG");
    REQUIRE(slots[2].color == "#0000FF");
    REQUIRE(slots[2].type == "ABS");
    REQUIRE(slots[3].color == "#FFFFFF");
    REQUIRE(slots[3].type == "TPU");
}

TEST_CASE("CFS boxsInfo parsing ignores non-target payloads", "[CFS]")
{
    // A heartbeat-style reply carries no box materials.
    nlohmann::json heartbeat = {{"ModeCode", "heart_beat"}};
    std::vector<SlotMaterial> slots(kSlotCount);
    REQUIRE_FALSE(extract_materials_from_json(heartbeat, slots));
}

TEST_CASE("CFS modifyMaterial payload", "[CFS]")
{
    SlotMaterial slot;
    slot.box_id      = 1;
    slot.material_id = 2;
    slot.rfid        = "00013";
    slot.type        = "PETG";
    slot.vendor      = "Generic";
    slot.name        = "Generic PETG";

    const auto payload = build_modify_material_payload(slot, "#112233");
    REQUIRE(payload["boxId"] == 1);
    REQUIRE(payload["id"] == 2);
    REQUIRE(payload["type"] == "PETG");
    REQUIRE(payload["color"] == "#0112233");
}

TEST_CASE("CFS host extraction from print_host", "[CFS]")
{
    REQUIRE(extract_host_from_print_host("http://192.168.1.50") == "192.168.1.50");
    REQUIRE(extract_host_from_print_host("192.168.1.50:8080/path") == "192.168.1.50");
    REQUIRE(extract_host_from_print_host("https://user:pass@printer.local/") == "printer.local");
    REQUIRE(extract_host_from_print_host("[fe80::1]:80") == "fe80::1");
    REQUIRE(extract_host_from_print_host("   ").empty());
}
