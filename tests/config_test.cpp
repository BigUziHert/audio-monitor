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
    app.processPath = L"C:\\Apps\\Discord.exe";
    app.enabled = false;
    c.sources.push_back(app);
    c.mono = true;
    c.closeToTray = true;
    auto copy = Config::fromJson(JsonValue::parse(c.toJson().dump()));
    check(copy.sources.size() == 4 && copy.sources[3].processPath == app.processPath &&
              !copy.sources[3].enabled,
          "app identity and enabled state persist");
    check(copy.mono && copy.closeToTray && copy.output.muted, "mix and window preferences persist");
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
