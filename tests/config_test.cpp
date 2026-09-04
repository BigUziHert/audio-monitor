#include "config/Config.h"
#include "config/Json.h"
#include <cstdio>
using namespace audiomon;
int main() {
    int failed = 0;
    auto check = [&](bool ok, const char *label) {
        if (!ok) {
            std::printf("FAIL: %s\n", label);
            ++failed;
        }
    };
    for (const char* text : {
             R"({"gain":1e})", R"({"gain":.})", R"({"gain":+1})", R"({"gain":01})",
             R"({"gain":1.})", R"({"gain":1e+})", R"({"gain":1-2})", R"({"gain":1.2.3})",
             R"({"gain":1e9999})", R"({"label":"\uZZZZ"})", R"({"label":"\u12x4"})",
             R"({"label":"\uD800"})", R"({"label":"\uDC00"})", R"({"label":"\uD800\u0041"})",
             "{\"label\":\"raw\nnewline\"}"}) {
        std::string error;
        const auto invalid = JsonValue::parse(text, &error);
        check(invalid.isNull() && !error.empty(), text);
    }
    std::string parseError;
    const auto valid = JsonValue::parse(
        R"({"negative":-0.125,"exponent":2.5E+1,"label":"\u0041\uD83D\uDD0A"})", &parseError);
    check(parseError.empty() && valid.find("negative") &&
              valid.find("negative")->asNumber(0) == -0.125 && valid.find("exponent") &&
              valid.find("exponent")->asNumber(0) == 25,
          "valid fractions and exponents parse");
    check(valid.find("label") && valid.find("label")->asString("") == "A\xF0\x9F\x94\x8A",
          "Unicode escapes and surrogate pairs decode to UTF-8");
    auto legacy = JsonValue::parse(
        R"({"game":{"deviceId":"game-pin","gain":0.8,"muted":true},"chat":{"gain":0.7},"mic":{"gain":2.1},"output":{"muted":true},"bufferMillis":90})");
    auto c = Config::fromJson(legacy);
    check(c.sources.size() == 3, "legacy keeps three sources");
    check(c.sources[0].deviceId == L"game-pin" && c.sources[0].muted && c.sources[0].gain == 0.8f,
          "legacy preserves pin, gain, mute");
    check(c.sources[2].kind == SourceKind::Microphone && c.sources[2].gain == 2.1f,
          "legacy microphone retains boost");
    check(c.output.muted && c.bufferMillis == 90, "legacy master mute and buffer");
    ChannelConfig app;
    app.kind = SourceKind::Application;
    app.label = "Discord";
    app.icon = "chat";
    app.processPath = L"C:\\Apps\\Discord.exe";
    app.enabled = false;
    c.sources.push_back(app);
    c.mono = true;
    c.closeToTray = true;
    c.output.label = "Stream Output";
    c.output.icon = "screen";
    c.output.gain = 1.75f;
    auto copy = Config::fromJson(JsonValue::parse(c.toJson().dump()));
    check(copy.sources.size() == 4 && copy.sources[3].processPath == app.processPath &&
              copy.sources[3].icon == "chat" &&
              !copy.sources[3].enabled,
          "app identity, icon, and enabled state persist");
    check(copy.mono && copy.closeToTray && copy.output.muted, "mix and window preferences persist");
    check(copy.output.label == "Stream Output" && copy.output.icon == "screen" &&
              copy.output.gain == 1.75f,
          "output name, icon, and gain persist");
    c.sources.clear();
    copy = Config::fromJson(c.toJson());
    check(copy.sources.empty(), "empty source list remains empty");
    auto malformed =
        Config::fromJson(JsonValue::parse(R"({"sources":[{},false,{"gain":999}],"bufferMillis":999})"));
    check(malformed.sources.size() == 2 && malformed.sources[1].gain == 4 && malformed.bufferMillis == 250,
          "invalid entries skipped and gains clamped");
    auto many = JsonValue::object(), list = JsonValue::array();
    for (int i = 0; i < 50; ++i)
        list.push(JsonValue::object());
    many.set("sources", list);
    check(Config::fromJson(many).sources.size() == kMaxSources, "source count bounded");
    std::printf("Configuration tests: %d failures\n", failed);
    return failed ? 1 : 0;
}
