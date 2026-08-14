#include "Profiles.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Hardcoded default profiles.
//
// These are the factory reset source of truth — they are never modified at
// runtime. The firmware reads from /profiles.json on LittleFS during normal
// operation. Factory reset simply rewrites that file from these constants.
//
// Placeholder codes (0x00000000) indicate the correct discrete codes have
// not yet been confirmed for that manufacturer. The profile will be visible
// in the cycle but IR transmission will have no effect until real codes are
// added via the web UI.
// ---------------------------------------------------------------------------
struct DefaultProfile {
    const char* name;
    const char* protocol;
    uint32_t    onCode;
    uint32_t    offCode;
    bool        visible;
};

static const DefaultProfile DEFAULT_PROFILES[] = {
    { "LG",      "NEC",     0x20DF23DC, 0x20DFA35C, true },  // Confirmed on LG C2
    { "Samsung", "SAMSUNG", 0x00000000, 0x00000000, true },  // Placeholder
    { "Sony",    "SONY",    0x00000000, 0x00000000, true },  // Placeholder
    { "TCL",     "NEC",     0x00000000, 0x00000000, true },  // Placeholder
    { "Hisense", "NEC",     0x00000000, 0x00000000, true },  // Placeholder
};

static const Settings DEFAULT_SETTINGS = {
    .activeProfile   = 0,      // LG selected by default
    .displayAlwaysOn = false,  // Display off when idle by default
};

// ---------------------------------------------------------------------------
// Module state — loaded into memory by begin(), updated by save functions
// ---------------------------------------------------------------------------
static std::vector<Profile> profiles_;
static Settings              settings_;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Format a uint32_t code as a "0x" hex string into a caller-supplied buffer.
static void codeToHex(uint32_t code, char* buf, size_t len) {
    snprintf(buf, len, "0x%08X", code);
}

// Serialise a document to `path` without ever leaving a half-written file
// behind.
//
// Opening the destination with "w" truncates it immediately, so losing power
// mid-write left a truncated file that failed to parse on the next boot. Every
// profile cycle triggers a save, so that window was hit often enough to matter.
// Writing to a temporary file first and renaming means the destination is only
// ever replaced once the new contents are safely on flash — a power loss leaves
// either the old file or the new one, never a partial one.
static bool writeJsonAtomic(const char* path, const JsonDocument& doc) {
    const String tmpPath = String(path) + ".tmp";

    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) {
        Serial.printf("[profiles] Failed to open %s for write\n", tmpPath.c_str());
        return false;
    }

    const size_t written = serializeJson(doc, f);
    f.close();

    if (written == 0) {
        Serial.printf("[profiles] Wrote 0 bytes to %s — discarding\n", tmpPath.c_str());
        LittleFS.remove(tmpPath.c_str());
        return false;
    }

    // littlefs replaces the destination atomically, but some ports refuse to
    // rename onto an existing path — fall back to an explicit remove.
    // c_str() on both arguments avoids an ambiguous String/const char* overload.
    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        LittleFS.remove(path);
        if (!LittleFS.rename(tmpPath.c_str(), path)) {
            Serial.printf("[profiles] Failed to rename %s onto %s\n", tmpPath.c_str(), path);
            LittleFS.remove(tmpPath.c_str());
            return false;
        }
    }

    return true;
}

// Serialise the hardcoded default profiles into a document.
static void buildDefaultProfilesDoc(JsonDocument& doc) {
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& d : DEFAULT_PROFILES) {
        JsonObject obj = arr.add<JsonObject>();
        char onBuf[11], offBuf[11];
        codeToHex(d.onCode,  onBuf,  sizeof(onBuf));
        codeToHex(d.offCode, offBuf, sizeof(offBuf));
        obj["name"]     = d.name;
        obj["protocol"] = d.protocol;
        obj["on"]       = onBuf;
        obj["off"]      = offBuf;
        obj["visible"]  = d.visible;
    }
}

// Write the hardcoded default profiles to /profiles.json.
static void writeDefaultProfiles() {
    JsonDocument doc;
    buildDefaultProfilesDoc(doc);
    writeJsonAtomic("/profiles.json", doc);
}

// Write the hardcoded default settings to /settings.json.
static void writeDefaultSettings() {
    JsonDocument doc;
    doc["active_profile"]    = DEFAULT_SETTINGS.activeProfile;
    doc["display_always_on"] = DEFAULT_SETTINGS.displayAlwaysOn;
    writeJsonAtomic("/settings.json", doc);
}

// Read /profiles.json into profiles_.
static void loadProfiles() {
    profiles_.clear();

    File f = LittleFS.open("/profiles.json", "r");
    if (!f) { Serial.println("[profiles] profiles.json not found"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[profiles] JSON parse error: %s\n", err.c_str());
        return;
    }

    for (JsonObjectConst obj : doc.as<JsonArrayConst>()) {
        profiles_.push_back(Profiles::fromJson(obj));
    }

    Serial.printf("[profiles] Loaded %d profiles\n", (int)profiles_.size());
}

// Read /settings.json into settings_.
static void loadSettings() {
    settings_ = DEFAULT_SETTINGS;  // Start from defaults so missing fields are safe

    File f = LittleFS.open("/settings.json", "r");
    if (!f) { Serial.println("[profiles] settings.json not found"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[profiles] Settings JSON parse error: %s\n", err.c_str());
        return;
    }

    // The | operator provides a fallback value if the field is missing
    settings_.activeProfile   = doc["active_profile"]    | DEFAULT_SETTINGS.activeProfile;
    settings_.displayAlwaysOn = doc["display_always_on"] | DEFAULT_SETTINGS.displayAlwaysOn;

    // Clamp active profile index to the valid range
    if (settings_.activeProfile < 0 ||
        settings_.activeProfile >= (int)profiles_.size()) {
        settings_.activeProfile = 0;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace Profiles {

bool begin() {
    // true = format LittleFS if mounting fails (e.g. first flash after partition change)
    if (!LittleFS.begin(true)) {
        Serial.println("[profiles] LittleFS mount failed — running on fallback profile");
        return false;
    }

    // Write defaults on first boot if files do not exist
    if (!LittleFS.exists("/profiles.json")) {
        Serial.println("[profiles] First boot — writing default profiles");
        writeDefaultProfiles();
    }
    if (!LittleFS.exists("/settings.json")) {
        Serial.println("[profiles] First boot — writing default settings");
        writeDefaultSettings();
    }

    loadProfiles();

    // A present but unreadable profiles.json (corrupt, truncated, or hand-edited
    // down to an empty array) leaves nothing to select. Rewrite the defaults and
    // retry rather than continuing with no profiles at all.
    if (profiles_.empty()) {
        Serial.println("[profiles] No profiles loaded — restoring defaults");
        writeDefaultProfiles();
        loadProfiles();
    }

    // Load settings after profiles so the active index can be range-checked.
    loadSettings();

    Serial.printf("[profiles] Active profile: %d (%s)\n",
                  settings_.activeProfile,
                  getActive().name.c_str());
    return !profiles_.empty();
}

const std::vector<Profile>& getAll()     { return profiles_; }

const Profile& getActive() {
    // Returned by reference, so this must stay valid even with no profiles
    // loaded — a failed LittleFS mount leaves the list empty and indexing it
    // would read out of bounds. The fallback keeps the device responsive
    // (display, button, HID all still work) on the one confirmed profile.
    static const Profile FALLBACK = {
        String(DEFAULT_PROFILES[0].name),
        protocolFromString(String(DEFAULT_PROFILES[0].protocol)),
        DEFAULT_PROFILES[0].onCode,
        DEFAULT_PROFILES[0].offCode,
        true
    };

    if (profiles_.empty()) return FALLBACK;

    // Defensive: settings_.activeProfile is clamped on load and on every write
    // path, but a stale index here would be an out-of-bounds read.
    if (settings_.activeProfile < 0 ||
        settings_.activeProfile >= (int)profiles_.size()) {
        return profiles_[0];
    }

    return profiles_[settings_.activeProfile];
}

const Settings&              getSettings()        { return settings_; }
Settings&                    getMutableSettings()  { return settings_; }

int nextVisibleIndex() {
    int n = (int)profiles_.size();
    if (n == 0) return 0;

    int start = settings_.activeProfile;
    // Walk forward through the list (wrapping) looking for the next visible profile
    for (int i = 1; i < n; i++) {
        int idx = (start + i) % n;
        if (profiles_[idx].visible) return idx;
    }
    // No other visible profiles found — stay on current
    return start;
}

void replaceAll(std::vector<Profile> newProfiles) {
    profiles_ = std::move(newProfiles);
    if (settings_.activeProfile >= (int)profiles_.size()) {
        settings_.activeProfile = 0;
    }
}

void saveProfiles() {
    JsonDocument doc;
    JsonArray    arr = doc.to<JsonArray>();

    for (const auto& p : profiles_) {
        JsonObject obj = arr.add<JsonObject>();
        char onBuf[11], offBuf[11];
        codeToHex(p.onCode,  onBuf,  sizeof(onBuf));
        codeToHex(p.offCode, offBuf, sizeof(offBuf));
        obj["name"]     = p.name;
        obj["protocol"] = protocolToString(p.protocol);
        obj["on"]       = onBuf;
        obj["off"]      = offBuf;
        obj["visible"]  = p.visible;
    }

    if (writeJsonAtomic("/profiles.json", doc)) {
        Serial.println("[profiles] Saved profiles.json");
    }
}

void saveSettings() {
    JsonDocument doc;
    doc["active_profile"]    = settings_.activeProfile;
    doc["display_always_on"] = settings_.displayAlwaysOn;

    if (writeJsonAtomic("/settings.json", doc)) {
        Serial.println("[profiles] Saved settings.json");
    }
}

void factoryReset() {
    Serial.println("[profiles] Factory reset — restoring defaults");
    writeDefaultProfiles();
    writeDefaultSettings();
    loadProfiles();
    loadSettings();
    Serial.println("[profiles] Factory reset complete");
}

Profile fromJson(JsonObjectConst obj) {
    Profile p;
    p.name     = obj["name"].as<String>();
    p.protocol = protocolFromString(obj["protocol"].as<String>());

    // Codes are stored as hex strings ("0x20DF23DC") — parse back to uint32_t.
    // as<const char*>() yields nullptr when the key is missing or is not a
    // string, and strtoul(nullptr, ...) is undefined behaviour, so both are
    // checked before parsing. An unset code becomes 0x0, matching the
    // "not yet configured" placeholder convention used by the defaults.
    const char* onStr  = obj["on"].as<const char*>();
    const char* offStr = obj["off"].as<const char*>();
    p.onCode   = onStr  ? (uint32_t)strtoul(onStr,  nullptr, 16) : 0;
    p.offCode  = offStr ? (uint32_t)strtoul(offStr, nullptr, 16) : 0;

    p.visible  = obj["visible"] | true;  // Default to visible if field missing
    return p;
}

IrProtocol protocolFromString(const String& s) {
    if (s == "SAMSUNG") return IrProtocol::SAMSUNG;
    if (s == "SONY")    return IrProtocol::SONY;
    return IrProtocol::NEC;  // NEC is the default — covers LG, TCL, Hisense
}

String protocolToString(IrProtocol p) {
    switch (p) {
        case IrProtocol::SAMSUNG: return "SAMSUNG";
        case IrProtocol::SONY:    return "SONY";
        default:                  return "NEC";
    }
}

} // namespace Profiles
