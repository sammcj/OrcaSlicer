#ifndef slic3r_CrealityCFS_hpp_
#define slic3r_CrealityCFS_hpp_

// Creality CFS (Filament System) integration.
//
// Non-UI logic for talking to a Creality printer's CFS over a WebSocket. This
// header is intentionally free of any wxWidgets dependency so the parsing and
// payload helpers can be unit tested without the GUI. The Sidebar (Plater.cpp)
// owns the SocketRuntime and drives the UI from its snapshots.
//
// CFS WebSocket protocol:
//   endpoint   ws://<printer-ip>:9999
//   heartbeat  {"ModeCode":"heart_beat"}  -> peer echoes "heart_beat", we reply "ok"
//   poll       {"method":"get","params":{"boxsInfo":1}}  -> boxsInfo payload
//   write      {"method":"set","params":{"modifyMaterial":{...}}}
// Colours arrive as 7-hex "#0RRGGBB"; we normalise to "#RRGGBB" and re-encode
// to the printer form on write.

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace CrealityCFS {

using json = nlohmann::json;

constexpr std::size_t kSlotCount      = 4;
constexpr int         kPollIntervalMs = 3000;
constexpr int         kRetryMs        = 1200;
constexpr int         kHeartbeatMs    = 10000;
constexpr int         kIdleSleepMs    = 100;
constexpr std::chrono::seconds kSocketTimeout{2};
constexpr std::chrono::seconds kRecentSuccessWindow{15};

// app_config keys (single source of truth). Absent => feature off.
constexpr const char* kCfgAutoSync        = "cfs_color_auto_sync";
constexpr const char* kCfgAutoApplyPreset = "cfs_auto_apply_filament_preset";
constexpr const char* kCfgPresetMapPrefix = "cfs_preset_map_";

// Static mapping from a CFS material type to the metadata the printer expects
// when a preset is pushed back to the CFS (modifyMaterial).
struct MaterialPresetDefinition
{
    const char* type;
    const char* rfid;
    const char* vendor;
    const char* name;
    const char* temp_min;
    const char* temp_max;
    const char* pressure;
};

// A single CFS slot's material, parsed from boxsInfo.
struct SlotMaterial
{
    int         box_id{1};
    int         material_id{-1};
    std::string rfid;
    std::string color;
    std::string type;
    std::string vendor;
    std::string name;
    json        min_temp;
    json        max_temp;
    json        pressure;
};

// --- pure helpers (no wx, unit-testable) ---

// Normalise a CFS colour to "#RRGGBB" upper-case, or {} when unparseable.
std::string normalize_color(const std::string& raw_color);
// Trim + upper-case a material type.
std::string normalize_material_type(const std::string& raw_type);
// Re-encode a normalised "#RRGGBB" colour to the printer's "#0RRGGBB" form.
std::string encode_printer_color(const std::string& raw_color);
// Recursively walk a boxsInfo JSON tree and fill the slot vector. Returns true
// when a target box with material data was found. Bounded recursion depth.
bool extract_materials_from_json(const json& node, std::vector<SlotMaterial>& slot_materials, int depth = 0);
// Build a modifyMaterial payload for a slot with a new colour.
json build_modify_material_payload(const SlotMaterial& slot, const std::string& color);
// Extract a bare host (no scheme/port/path/credentials) from a print_host URL.
std::string extract_host_from_print_host(const std::string& print_host);

const MaterialPresetDefinition* find_material_preset_definition(const std::string& material_type);
std::vector<std::string>        supported_material_types();
std::string                     material_type_config_key(const std::string& material_type);
std::string                     default_preset_mapping(const std::string& material_type);

// Persistent CFS session. Thread-safe: run() executes on a worker thread, all
// other methods are safe to call from the UI thread. The mutex is private so
// callers never touch the runtime's internals directly.
class SocketRuntime
{
public:
    SocketRuntime(std::string host, std::string origin);

    // Worker-thread entry point. Loops (connect, poll, read, reconnect) until
    // request_stop() is observed.
    void run();

    void request_stop();
    void request_boxs_info();

    struct Snapshot
    {
        bool                                  connected{false};
        bool                                  has_recent_success{false};
        std::chrono::steady_clock::time_point last_success_at{};
        std::vector<std::string>              colors;
        std::vector<std::string>              material_types;
        std::size_t                           generation{0};
    };
    Snapshot snapshot() const;
    bool     connected() const;

    // Queue an outbound modifyMaterial built from cached slot metadata + colour.
    bool queue_modify_material(std::size_t slot_idx, const std::string& color);
    // Queue a fully-formed modifyMaterial payload.
    bool queue_modify_material_payload(json payload);
    // Copy cached metadata for a slot. Returns false when unavailable.
    bool slot_material(std::size_t slot_idx, SlotMaterial& out) const;

    const std::string& host() const { return m_host; }
    const std::string& origin() const { return m_origin; }

private:
    bool              should_stop() const;
    void              set_connected(bool connected);
    bool              consume_boxs_info_request();
    void              store_materials(std::vector<SlotMaterial> slot_materials);
    std::vector<json> consume_pending_modify_materials();

    mutable std::mutex m_mutex;
    const std::string  m_host;
    const std::string  m_origin;
    bool               m_connected{false};
    bool               m_stop_requested{false};
    bool               m_request_boxs_info{false};
    bool               m_has_recent_success{false};
    std::vector<std::string>              m_colors;
    std::vector<std::string>              m_material_types;
    std::vector<SlotMaterial>             m_slot_materials = std::vector<SlotMaterial>(kSlotCount);
    std::vector<json>                     m_pending_modify_materials;
    std::chrono::steady_clock::time_point m_last_success_at{};
    std::size_t                           m_generation{0};
};

}} // namespace Slic3r::CrealityCFS

#endif // slic3r_CrealityCFS_hpp_
