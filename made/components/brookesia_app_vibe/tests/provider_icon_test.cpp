// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include "provider_catalog.hpp"

using namespace vibe_provider;
Icon parse(const std::string &json) {
    auto *root = cJSON_Parse(json.c_str());
    assert(root);
    auto icon = parseIcon(root);
    cJSON_Delete(root);
    return icon;
}
std::string replaceText(std::string text, const std::string &a, const std::string &b) {
    const auto pos = text.find(a); assert(pos != std::string::npos);
    return text.replace(pos, a.size(), b);
}
int main(int argc, char **argv) {
    const std::string two = R"({"format":"indexed4","width":2,"height":1,"palette":["FF2211FF","4488BB80"],"data":"AQ==","background":"#012345","accent":"#aBcDeF"})";
    auto icon = parse(two);
    assert(icon.valid() && icon.width == 2 && icon.height == 1 && icon.packed[0] == 1);
    assert(icon.background == 0x012345 && icon.accent == 0xabcdef);
    std::array<uint8_t, MAX_ICON_PIXELS * 4> pixels{};
    assert(decodeIconBgra(icon, pixels.data(), pixels.size()));
    assert(pixels[0] == 0x11 && pixels[1] == 0x22 && pixels[2] == 0xff && pixels[3] == 0xff);
    assert(pixels[4] == 0xbb && pixels[5] == 0x88 && pixels[6] == 0x44 && pixels[7] == 0x80);
    assert(!decodeIconBgra(icon, pixels.data(), 7));
    assert(decodeIconCanvasBgra(icon, pixels.data(), pixels.size()));
    assert(pixels[3] == 0); // The 2:1 image is centered with transparent letterboxing.
    assert(pixels[(24 * 48 + 4) * 4 + 2] == 0xff);
    assert(pixels[(24 * 48 + 42) * 4 + 3] == 0x80);
    const auto one = replaceText(replaceText(two, "\"width\":2", "\"width\":1"), "AQ==", "EA==");
    assert(parse(one).valid()); // Pixel 1 in high nibble, padding zero.
    assert(!parse(replaceText(one, "EA==", "EQ==")).valid());
    assert(!parse(replaceText(two, "AQ==", "Ag==")).valid()); // Palette index 2 out of range.
    assert(!parse(replaceText(two, "AQ==", "AR==")).valid()); // Noncanonical base64 trailing bits.
    assert(!parse(replaceText(two, "AQ==", "AQ=+")).valid());
    assert(!parse(replaceText(two, "AQ==", "AQ===")).valid());
    assert(!parse(replaceText(two, "AQ==", "AQ")).valid());
    assert(!parse(replaceText(two, "AQ==", "AQ\\n==")).valid());
    for (const auto &width : {"0", "-1", "49", "2.1", "null", "\"2\""})
        assert(!parse(replaceText(two, "\"width\":2", std::string("\"width\":") + width)).valid());
    assert(!parse(replaceText(two, "indexed4", "png")).valid());
    assert(!parse(replaceText(two, "FF2211FF", "#FF2211FF")).valid());
    assert(!parse(replaceText(two, "FF2211FF", "FF2211")).valid());
    assert(!parse(replaceText(two, "#012345", "#GGGGGG")).valid());
    assert(!parse("null").valid() && !parse("\"fox\"").valid() && !parse("{}").valid());
    // One broken image does not discard an otherwise valid assistant catalog.
    auto *root = cJSON_Parse(("[{\"id\":\"custom\",\"label\":\"My icon\",\"icon\":" + two + "},{\"id\":\"no-image\",\"icon\":{\"format\":\"png\"}}]").c_str());
    std::vector<Provider> catalog;
    assert(vibe_provider::parse(root, catalog) && catalog.size() == 2);
    cJSON_Delete(root);
    assert(catalog[0].icon == icon && !catalog[1].icon.valid());
    catalog[0].selected_session_id = "keep-this-session";
    auto next = catalog;
    next[0].icon = parse(replaceText(two, "FF2211FF", "00FF00FF"));
    assert(!(next[0].icon == icon));
    replace(catalog, next, "custom");
    assert(catalog[0].selected_session_id == "keep-this-session" && !(catalog[0].icon == icon));
    if (argc > 1) {
        std::ifstream stream(argv[1]);
        std::stringstream json; json << stream.rdbuf();
        auto *bridge = cJSON_Parse(json.str().c_str()); assert(bridge && cJSON_IsArray(bridge));
        assert(vibe_provider::parse(bridge, catalog) && catalog.size() == 5);
        for (const auto &provider : catalog) {
            assert(provider.icon.valid() && provider.icon.width == 48 && provider.icon.height == 48);
            assert(provider.icon.packed.size() == 1152);
            assert(decodeIconBgra(provider.icon, pixels.data(), pixels.size()));
        }
        cJSON_Delete(bridge);
    }
    std::cout << "provider icon tests passed: pixels/RGBA, strict bounds/base64/palette, invalid isolation, same-ID replacement, real bridge fixtures\n";
}
