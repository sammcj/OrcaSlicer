#include "CrealityCFS.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <thread>
#include <unordered_set>

#include <boost/algorithm/string.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace CrealityCFS {

namespace {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

constexpr int kMaxJsonDepth = 64;

constexpr MaterialPresetDefinition kMaterialPresetDefinitions[] = {
    {"PLA",      "00001", "Generic", "Generic PLA",       "190", "240", "0.04"},
    {"PLA-SILK", "00005", "Generic", "Generic PLA-Silk",  "190", "240", "0.052"},
    {"PETG",     "00013", "Generic", "Generic PETG",      "220", "270", "0.08"},
    {"ABS",      "00003", "Generic", "Generic ABS",       "240", "280", "0.06"},
    {"ASA",      "00004", "Generic", "Generic ASA",       "240", "280", "0.044"},
    {"BVOH",     "00007", "Generic", "Generic BVOH",      "200", "220", "0.02"},
    {"HIPS",     "00010", "Generic", "Generic HIPS",      "220", "250", "0.02"},
    {"PA",       "00012", "Generic", "Generic PA",        "240", "260", "0.01"},
    {"PA-CF",    "00008", "Generic", "Generic PA-CF",     "260", "300", "0.01"},
    {"PC",       "00009", "Generic", "Generic PC",        "250", "270", "0.05"},
    {"PET",      "00021", "Generic", "Generic PET",       "250", "270", "0.02"},
    {"PET-CF",   "00020", "Generic", "Generic PET-CF",    "280", "320", "0.02"},
    {"PETG-CF",  "00003", "Generic", "Generic PETG-CF",   "240", "260", "0.02"},
    {"PLA-CF",   "00001", "Generic", "Generic PLA-CF",    "190", "240", "0.03"},
    {"PA6-CF",   "",      "Generic", "Generic PA6-CF",    "280", "300", "0.02"},
    {"PAHT-CF",  "",      "Generic", "Generic PAHT-CF",   "300", "320", "0.02"},
    {"PP",       "00006", "Generic", "Generic PP",        "220", "260", "0.02"},
    {"PPS",      "00019", "Generic", "Generic PPS",       "320", "350", "0.02"},
    {"PPS-CF",   "00017", "Generic", "Generic PPS-CF",    "300", "350", "0.02"},
    {"PVA",      "00005", "Generic", "Generic PVA",       "215", "225", "0.02"},
    {"TPU",      "00011", "Generic", "Generic TPU",       "210", "240", "0.02"},
    {"OTHER",    "",      "Generic", "Generic Material",  "200", "240", "0.04"},
};

json json_value_or_null(const json& material, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const auto it = material.find(key);
        if (it != material.end() && !it->is_null())
            return *it;
    }
    return nullptr;
}

template<typename WebsocketT>
void send_json(WebsocketT& ws, const json& payload)
{
    ws.write(net::buffer(payload.dump()));
}

template<typename WebsocketT>
void send_text(WebsocketT& ws, const std::string& payload)
{
    ws.write(net::buffer(payload));
}

} // namespace

std::string normalize_color(const std::string& raw_color)
{
    std::string hex;
    hex.reserve(raw_color.size());

    for (unsigned char ch : raw_color) {
        if (std::isxdigit(ch))
            hex += static_cast<char>(std::toupper(ch));
    }

    if (hex.size() == 7 && hex.front() == '0')
        hex = hex.substr(1);
    else if (hex.size() == 8)
        hex = hex.substr(2);
    else if (hex.size() > 8)
        hex = hex.substr(hex.size() - 6);

    if (hex.size() != 6)
        return {};

    return "#" + hex;
}

std::string normalize_material_type(const std::string& raw_type)
{
    std::string normalized = raw_type;
    boost::algorithm::trim(normalized);
    boost::algorithm::to_upper(normalized);
    return normalized;
}

std::string encode_printer_color(const std::string& raw_color)
{
    const std::string normalized = normalize_color(raw_color);
    if (normalized.empty())
        return {};

    return "#0" + normalized.substr(1);
}

json build_modify_material_payload(const SlotMaterial& slot, const std::string& color)
{
    return json{
        {"boxId",    slot.box_id},
        {"id",       slot.material_id},
        {"rfid",     slot.rfid},
        {"type",     slot.type},
        {"vendor",   slot.vendor},
        {"name",     slot.name},
        {"color",    encode_printer_color(color)},
        {"minTemp",  slot.min_temp.is_null() ? json(nullptr) : slot.min_temp},
        {"maxTemp",  slot.max_temp.is_null() ? json(nullptr) : slot.max_temp},
        {"pressure", slot.pressure.is_null() ? json("") : slot.pressure}
    };
}

const MaterialPresetDefinition* find_material_preset_definition(const std::string& material_type)
{
    for (const auto& definition : kMaterialPresetDefinitions) {
        if (material_type == definition.type)
            return &definition;
    }
    return nullptr;
}

std::vector<std::string> supported_material_types()
{
    std::vector<std::string> result;
    result.reserve(std::size(kMaterialPresetDefinitions));
    std::unordered_set<std::string> seen;
    for (const auto& definition : kMaterialPresetDefinitions) {
        if (seen.insert(definition.type).second)
            result.emplace_back(definition.type);
    }
    return result;
}

std::string material_type_config_key(const std::string& material_type)
{
    if (material_type == "PLA")
        return std::string(kCfgPresetMapPrefix) + "pla";
    if (material_type == "PETG")
        return std::string(kCfgPresetMapPrefix) + "petg";

    std::string suffix;
    suffix.reserve(material_type.size());
    for (const unsigned char ch : material_type) {
        if (std::isalnum(ch))
            suffix.push_back(static_cast<char>(std::tolower(ch)));
        else
            suffix.push_back('_');
    }
    return std::string(kCfgPresetMapPrefix) + suffix;
}

std::string default_preset_mapping(const std::string& /*material_type*/)
{
    // No built-in guess. A CFS material type maps to an Orca filament preset only
    // once the user configures it (Configure CFS dialog), so auto-apply never
    // targets a preset name that may not exist. Unconfigured types stay unmapped.
    return {};
}

bool extract_materials_from_json(const json& node, std::vector<SlotMaterial>& slot_materials, int depth)
{
    if (depth > kMaxJsonDepth)
        return false;

    if (node.is_object()) {
        const auto type_it      = node.find("type");
        const auto box_id_it    = node.find("id");
        const auto materials_it = node.find("materials");
        const bool is_target_box =
            type_it != node.end() &&
            ((type_it->is_number_integer() && type_it->get<int>() == 0) ||
             (type_it->is_string() && type_it->get<std::string>() == "0")) &&
            materials_it != node.end() && materials_it->is_array();

        // Surface the first type==0 box that carries material data, indexing its
        // slots by each material's own id (0..kSlotCount-1). A K-series CFS is a
        // single 4-slot box; chained multi-box setups are not represented.
        if (is_target_box) {
            std::vector<SlotMaterial> extracted_slots(kSlotCount);
            const int box_id = box_id_it != node.end() && box_id_it->is_number_integer() ? box_id_it->get<int>() : 1;
            bool      found_slot_data = false;

            for (const auto& material : *materials_it) {
                if (!material.is_object())
                    continue;

                const auto id_it = material.find("id");
                if (id_it == material.end() || !id_it->is_number_integer())
                    continue;

                const int slot_idx = id_it->get<int>();
                if (slot_idx < 0 || slot_idx >= static_cast<int>(kSlotCount))
                    continue;

                auto& extracted_slot = extracted_slots[slot_idx];
                extracted_slot.box_id      = box_id;
                extracted_slot.material_id = slot_idx;

                if (const auto rfid_it = material.find("rfid"); rfid_it != material.end() && rfid_it->is_string())
                    extracted_slot.rfid = rfid_it->get<std::string>();

                if (const auto color_it = material.find("color"); color_it != material.end() && color_it->is_string()) {
                    extracted_slot.color = normalize_color(color_it->get<std::string>());
                    found_slot_data = found_slot_data || !extracted_slot.color.empty();
                }

                if (const auto type_field_it = material.find("type"); type_field_it != material.end() && type_field_it->is_string()) {
                    extracted_slot.type = normalize_material_type(type_field_it->get<std::string>());
                    found_slot_data = found_slot_data || !extracted_slot.type.empty();
                }

                if (const auto vendor_it = material.find("vendor"); vendor_it != material.end() && vendor_it->is_string())
                    extracted_slot.vendor = vendor_it->get<std::string>();

                if (const auto name_it = material.find("name"); name_it != material.end() && name_it->is_string())
                    extracted_slot.name = name_it->get<std::string>();

                extracted_slot.min_temp = json_value_or_null(material, {"minTemp", "nozzleTempMin", "minPrintTemp"});
                extracted_slot.max_temp = json_value_or_null(material, {"maxTemp", "nozzleTempMax", "maxPrintTemp"});
                extracted_slot.pressure = json_value_or_null(material, {"pressure"});
            }

            if (found_slot_data) {
                slot_materials = std::move(extracted_slots);
                return true;
            }
        }

        for (auto it = node.begin(); it != node.end(); ++it) {
            if (extract_materials_from_json(it.value(), slot_materials, depth + 1))
                return true;
        }
    } else if (node.is_array()) {
        for (const auto& item : node) {
            if (extract_materials_from_json(item, slot_materials, depth + 1))
                return true;
        }
    }

    return false;
}

std::string extract_host_from_print_host(const std::string& print_host)
{
    std::string host = print_host;
    boost::algorithm::trim(host);

    if (host.empty())
        return {};

    if (!boost::algorithm::istarts_with(host, "http://") && !boost::algorithm::istarts_with(host, "https://"))
        host = "http://" + host;

    if (const auto scheme_pos = host.find("://"); scheme_pos != std::string::npos)
        host = host.substr(scheme_pos + 3);

    if (const auto slash_pos = host.find('/'); slash_pos != std::string::npos)
        host = host.substr(0, slash_pos);

    if (const auto at_pos = host.rfind('@'); at_pos != std::string::npos)
        host = host.substr(at_pos + 1);

    if (!host.empty() && host.front() == '[') {
        if (const auto end = host.find(']'); end != std::string::npos)
            return host.substr(1, end - 1);
        return {};
    }

    if (const auto colon_pos = host.rfind(':');
        colon_pos != std::string::npos && host.find(':') == colon_pos)
        host = host.substr(0, colon_pos);

    return host;
}

// ---------------------------------------------------------------------------
// SocketRuntime
// ---------------------------------------------------------------------------

SocketRuntime::SocketRuntime(std::string host, std::string origin)
    : m_host(std::move(host)), m_origin(std::move(origin))
{}

void SocketRuntime::request_stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop_requested = true;
}

void SocketRuntime::request_boxs_info()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_request_boxs_info = true;
}

bool SocketRuntime::should_stop() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stop_requested;
}

void SocketRuntime::set_connected(bool connected)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = connected;
}

bool SocketRuntime::connected() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected;
}

bool SocketRuntime::consume_boxs_info_request()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool should_request = m_request_boxs_info;
    m_request_boxs_info = false;
    return should_request;
}

void SocketRuntime::store_materials(std::vector<SlotMaterial> slot_materials)
{
    std::vector<std::string> colors(kSlotCount);
    std::vector<std::string> material_types(kSlotCount);
    for (std::size_t idx = 0; idx < std::min(slot_materials.size(), kSlotCount); ++idx) {
        colors[idx]         = slot_materials[idx].color;
        material_types[idx] = slot_materials[idx].type;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_slot_materials     = std::move(slot_materials);
    m_colors             = std::move(colors);
    m_material_types     = std::move(material_types);
    m_has_recent_success = true;
    m_last_success_at    = std::chrono::steady_clock::now();
    ++m_generation;
}

SocketRuntime::Snapshot SocketRuntime::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot snap;
    snap.connected          = m_connected;
    snap.has_recent_success = m_has_recent_success;
    snap.last_success_at    = m_last_success_at;
    snap.colors             = m_colors;
    snap.material_types     = m_material_types;
    snap.generation         = m_generation;
    return snap;
}

std::vector<json> SocketRuntime::consume_pending_modify_materials()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<json> pending = std::move(m_pending_modify_materials);
    m_pending_modify_materials.clear();
    return pending;
}

bool SocketRuntime::queue_modify_material_payload(json payload)
{
    if (payload.is_null() || !payload.is_object())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_modify_materials.emplace_back(std::move(payload));
    m_request_boxs_info = true;
    return true;
}

bool SocketRuntime::queue_modify_material(std::size_t slot_idx, const std::string& color)
{
    const std::string normalized_color = normalize_color(color);
    if (normalized_color.empty() || slot_idx >= kSlotCount)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (slot_idx >= m_slot_materials.size())
        return false;

    const auto& slot = m_slot_materials[slot_idx];
    if (slot.material_id < 0 || slot.type.empty())
        return false;

    m_pending_modify_materials.emplace_back(build_modify_material_payload(slot, normalized_color));
    m_request_boxs_info = true;
    return true;
}

bool SocketRuntime::slot_material(std::size_t slot_idx, SlotMaterial& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (slot_idx >= m_slot_materials.size())
        return false;
    out = m_slot_materials[slot_idx];
    return true;
}

void SocketRuntime::run()
{
    while (!should_stop()) {
        const std::string& host   = m_host;   // immutable after construction
        const std::string& origin = m_origin;
        bool session_received_payload = false;

        if (host.empty())
            break;

        try {
            BOOST_LOG_TRIVIAL(debug) << "CFS socket: connecting to ws://" << host << ":9999";
            net::io_context                      ioc;
            tcp::resolver                        resolver{ioc};
            websocket::stream<beast::tcp_stream> ws{ioc};

            const auto results = resolver.resolve(host, "9999");
            ws.next_layer().expires_after(kSocketTimeout);
            const auto endpoint        = ws.next_layer().connect(results);
            const auto handshake_host  = host + ":" + std::to_string(endpoint.port());
            ws.next_layer().expires_never();

            ws.set_option(websocket::stream_base::decorator(
                [&origin](websocket::request_type& req) {
                    req.set(beast::http::field::user_agent, "OrcaSlicer");
                    if (!origin.empty())
                        req.set(beast::http::field::origin, origin);
                }));
            ws.handshake(handshake_host, "/");

            set_connected(true);
            BOOST_LOG_TRIVIAL(debug) << "CFS socket: connected to ws://" << host << ":9999";

            send_json(ws, json{{"ModeCode", "heart_beat"}});
            send_json(ws, json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});

            auto next_poll      = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPollIntervalMs);
            auto next_heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeartbeatMs);

            while (!should_stop()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_heartbeat) {
                    send_json(ws, json{{"ModeCode", "heart_beat"}});
                    next_heartbeat = now + std::chrono::milliseconds(kHeartbeatMs);
                }

                if (now >= next_poll || consume_boxs_info_request()) {
                    send_json(ws, json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});
                    next_poll = now + std::chrono::milliseconds(kPollIntervalMs);
                }

                for (const auto& modify_material : consume_pending_modify_materials()) {
                    BOOST_LOG_TRIVIAL(debug) << "CFS socket: sending modifyMaterial for slot "
                                             << modify_material.value("id", -1)
                                             << " color=" << modify_material.value("color", "");
                    send_json(ws, json{{"method", "set"}, {"params", {{"modifyMaterial", modify_material}}}});
                    send_json(ws, json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});
                    next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPollIntervalMs);
                }

                // Non-blocking drain: only read when bytes are buffered, otherwise
                // sleep briefly so heartbeat/poll/modify deadlines stay responsive.
                beast::error_code available_ec;
                const auto available_bytes = ws.next_layer().socket().available(available_ec);
                if (available_ec)
                    throw beast::system_error(available_ec);

                if (available_bytes == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMs));
                    continue;
                }

                // Bound the read. Bytes are buffered, but a peer that stalls
                // mid-frame must not block the worker indefinitely, or the
                // request_stop()/join() teardown on the UI thread would hang.
                // On timeout the stream is closed and we fall through to
                // reconnect; expires_never() restores the untimed steady state.
                beast::flat_buffer buffer;
                ws.next_layer().expires_after(kSocketTimeout);
                ws.read(buffer);
                ws.next_layer().expires_never();

                const auto payload = beast::buffers_to_string(buffer.data());
                session_received_payload = true;
                if (payload.find("heart_beat") != std::string::npos) {
                    send_text(ws, "ok");
                    continue;
                }

                if (payload == "ok")
                    continue;

                const auto parsed = json::parse(payload, nullptr, false);
                if (parsed.is_discarded())
                    continue;

                std::vector<SlotMaterial> slot_materials(kSlotCount);
                extract_materials_from_json(parsed, slot_materials);
                const bool has_materials = std::any_of(slot_materials.begin(), slot_materials.end(), [](const SlotMaterial& slot) {
                    return !slot.color.empty() || !slot.type.empty();
                });
                if (has_materials) {
                    store_materials(std::move(slot_materials));
                    set_connected(true);
                    BOOST_LOG_TRIVIAL(debug) << "CFS socket: read material info from " << host;
                }
            }

            beast::error_code close_ec;
            ws.close(websocket::close_code::normal, close_ec);
        } catch (const beast::system_error& ex) {
            const auto code = ex.code().value();
            const bool expected_disconnect =
                ex.code() == websocket::error::closed ||
                ex.code() == net::error::eof ||
                code == 995; // WSA_OPERATION_ABORTED on shutdown
            if (session_received_payload && expected_disconnect)
                BOOST_LOG_TRIVIAL(debug) << "CFS socket: session ended for " << host << " - " << ex.code().message();
            else
                BOOST_LOG_TRIVIAL(warning) << "CFS socket: failed for " << host << " - " << ex.what();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "CFS socket: failed for " << host << " - " << ex.what();
        }

        set_connected(false);
        if (should_stop())
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
    }
}

}} // namespace Slic3r::CrealityCFS
