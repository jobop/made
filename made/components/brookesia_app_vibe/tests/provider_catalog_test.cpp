// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <iostream>
#include "provider_catalog.hpp"

using namespace vibe_provider;

std::vector<Provider> parsed(const std::string &json)
{
    cJSON *root = cJSON_Parse(json.c_str());
    assert(root);
    std::vector<Provider> result;
    assert(parse(root, result));
    cJSON_Delete(root);
    return result;
}

int main()
{
    auto catalog = parsed(R"([
        {"id":"codex","label":"Codex","available":true},
        {"id":"cursor","label":"Cursor","available":true},
        {"id":"qoder","label":"Qoder","available":true}
    ])");
    catalog[0].selected_session_id = "codex-session";
    catalog[2].selected_session_id = "qoder-session";

    // A newly installed fifth plugin appears without a firmware lookup table.
    auto next = parsed(R"([
        {"id":"codex","available":true},
        {"id":"cursor","available":true},
        {"id":"qoder","available":true},
        {"id":"workbuddy","available":true},
        {"id":"custom-agent","label":"自定义助手","available":true,"icon":"owl",
         "capabilities":{"session":"native","model":true,"cancel":false,"progress":true}}
    ])");
    assert(next.size() == 5 && next[4].id == "custom-agent");
    assert(next[4].label == "自定义助手" && !next[4].icon.valid());
    assert(next[4].model && next[4].progress && !next[4].cancel && !next[4].external_unscoped);
    auto selected = replace(catalog, next, "qoder");
    assert(selected == 2 && catalog[selected].selected_session_id == "qoder-session");
    assert(catalog[3].external_unscoped && !catalog[3].icon.valid());

    // Bridge restart/reordering retains provider and session by ID, never index.
    next = parsed(R"([
        {"id":"custom-agent","label":"Changed name","available":true,"icon":"panda"},
        {"id":"qoder","available":true},
        {"id":"codex","available":false,"reason":"CLI unavailable"}
    ])");
    selected = replace(catalog, next, "qoder");
    assert(selected == 1 && catalog[selected].id == "qoder");
    assert(catalog[selected].selected_session_id == "qoder-session");
    assert(catalog[2].selected_session_id == "codex-session" && !catalog[2].available);
    assert(catalog[0].external_unscoped); // no session promise => conservative

    // Removal selects the first remaining ID and never copies another session.
    next = parsed(R"([{ "id":"custom-agent", "available":true, "icon":"new-style" }])");
    selected = replace(catalog, next, "qoder");
    assert(selected == 0 && catalog[0].id == "custom-agent");
    assert(catalog[0].selected_session_id.empty() && !catalog[0].icon.valid());
    assert(catalog[0].label == "custom-agent");

    // Invalid URL IDs and duplicates are rejected before being used in requests.
    next = parsed(R"([
        null, 7, {}, {"id":""}, {"id":"x&provider=codex"}, {"id":"UPPER"},
        {"id":"good-id","label":"First","available":true},
        {"id":"good-id","label":"Duplicate"},
        {"id":"valid_2","label":"Line\nBreak"}
    ])");
    assert(next.size() == 2 && next[0].label == "First" && next[1].label == "Line Break");
    assert(!next[1].available && !next[1].cancel);
    assert(!validId(std::string(65, 'a')));
    assert(validId(std::string(64, 'a')));

    // Bounded metadata never splits a Chinese UTF-8 character or grows unbounded.
    const std::string han = "声";
    std::string label;
    for (int i = 0; i < 40; ++i) label += han;
    next = parsed("[{\"id\":\"chinese\",\"label\":\"" + label + "\",\"reason\":\"" + std::string(400, 'x') + "\"}]");
    assert(next[0].label.size() == 63 && next[0].reason.size() == 240);
    for (size_t i = 0; i < next[0].label.size(); i += 3) assert(next[0].label.substr(i, 3) == han);

    // Large catalog is capped; malformed non-array does not replace good state.
    std::string many = "[";
    for (int i = 0; i < 20; ++i) {
        if (i) many += ',';
        many += "{\"id\":\"plugin-" + std::to_string(i) + "\",\"available\":true}";
    }
    many += ']';
    next = parsed(many);
    assert(next.size() == MAX_PROVIDERS && next.back().id == "plugin-11");
    cJSON *invalid = cJSON_Parse("{}");
    assert(!parse(invalid, next) && next.size() == MAX_PROVIDERS);
    cJSON_Delete(invalid);

    // A different bridge must start with a new namespace, even if its IDs match.
    catalog.clear();
    selected = replace(catalog, parsed(R"([{ "id":"custom-agent", "label":"Other PC" }])"), "custom-agent");
    assert(catalog[selected].selected_session_id.empty());
    // No built-in name/capability/icon fallback survives an ID-only response.
    next = parsed(R"([{ "id":"codex" }, { "id":"workbuddy" }])");
    assert(next[0].label == "codex" && next[0].external_unscoped && !next[0].model && !next[0].icon.valid());
    assert(next[1].label == "workbuddy" && !next[1].icon.valid());

    // Empty authoritative catalog is valid and causes an empty safe selection.
    selected = replace(catalog, parsed("[]"), "custom-agent");
    assert(selected == 0 && catalog.empty());
    selected = replace(catalog, parsed(R"([{ "id":"restored", "available":true }])"), "");
    assert(selected == 0 && catalog[0].id == "restored");
    std::cout << "provider catalog tests passed: dynamic add/reorder/remove, capabilities, bounds, UTF-8, malformed/empty/refresh\n";
}
