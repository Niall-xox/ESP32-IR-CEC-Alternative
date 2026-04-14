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

// Write the hardcoded default profiles to /profiles.json.
static void writeDefaultProfiles() {
    JsonDocument doc;
    JsonArray    arr = doc.to<JsonArray>();

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

    File f = LittleFS.open("/profiles.json", "w");
    if (!f) { Serial.println("[profiles] Failed to open profiles.json for write"); return; }
    serializeJson(doc, f);
    f.close();
}

// Write the hardcoded default settings to /settings.json.
static void writeDefaultSettings() {
    JsonDocument doc;
    doc["active_profile"]    = DEFAULT_SETTINGS.activeProfile;
    doc["display_always_on"] = DEFAULT_SETTINGS.displayAlwaysOn;

    File f = LittleFS.open("/settings.json", "w");
    if (!f) { Serial.println("[profiles] Failed to open settings.json for write"); return; }
    serializeJson(doc, f);
    f.close();
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

    for (JsonObject obj : doc.as<JsonArray>()) {
        Profile p;
        p.name     = obj["name"].as<String>();
        p.protocol = Profiles::protocolFromString(obj["protocol"].as<String>());
        // Codes are stored as hex strings ("0x20DF23DC") — parse back to uint32_t
        p.onCode   = strtoul(obj["on"].as<const char*>(),  nullptr, 16);
        p.offCode  = strtoul(obj["off"].as<const char*>(), nullptr, 16);
        p.visible  = obj["visible"] | true;  // Default to visible if field missing
        profiles_.push_back(p);
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
        Serial.println("[profiles] LittleFS mount failed");
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
    loadSettings();

    Serial.printf("[profiles] Active profile: %d (%s)\n",
                  settings_.activeProfile,
                  profiles_.empty() ? "none" : profiles_[settings_.activeProfile].name.c_str());
    return true;
}

const std::vector<Profile>& getAll()     { return profiles_; }
const Profile&               getActive() { return profiles_[settings_.activeProfile]; }
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

    File f = LittleFS.open("/profiles.json", "w");
    if (!f) { Serial.println("[profiles] Failed to save profiles.json"); return; }
    serializeJson(doc, f);
    f.close();
    Serial.println("[profiles] Saved profiles.json");
}

void saveSettings() {
    JsonDocument doc;
    doc["active_profile"]    = settings_.activeProfile;
    doc["display_always_on"] = settings_.displayAlwaysOn;

    File f = LittleFS.open("/settings.json", "w");
    if (!f) { Serial.println("[profiles] Failed to save settings.json"); return; }
    serializeJson(doc, f);
    f.close();
    Serial.println("[profiles] Saved settings.json");
}

void factoryReset() {
    Serial.println("[profiles] Factory reset — restoring defaults");
    writeDefaultProfiles();
    writeDefaultSettings();
    loadProfiles();
    loadSettings();
    Serial.println("[profiles] Factory reset complete");
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
