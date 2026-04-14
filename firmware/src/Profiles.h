#pragma once

// Profiles — manufacturer IR profile storage and LittleFS management.
//
// Profiles are stored as JSON in /profiles.json on LittleFS.
// Settings are stored in /settings.json.
// Both files are written from hardcoded defaults on first boot.
// Factory reset rewrites both files from those same defaults.
//
// A profile contains everything needed to send an IR command for a
// specific TV manufacturer: the display name, the IR protocol, and
// discrete ON/OFF codes. Discrete codes are preferred over toggle codes
// as they guarantee the correct TV state regardless of prior state.

#include <Arduino.h>
#include <vector>

// ---------------------------------------------------------------------------
// IR protocol enum
// Maps to the appropriate IRremoteESP8266 send function.
// Add new protocols here as more manufacturers are added.
// ---------------------------------------------------------------------------
enum class IrProtocol {
    NEC,      // Used by LG, TCL, Hisense and many others
    SAMSUNG,  // Samsung's own 32-bit protocol
    SONY      // Sony's protocol (variable bit length, defaults to 20-bit)
};

// ---------------------------------------------------------------------------
// Profile struct — one entry in the profiles list
// ---------------------------------------------------------------------------
struct Profile {
    String     name;      // Human-readable label shown on OLED and web UI
    IrProtocol protocol;  // IR protocol used to transmit codes
    uint32_t   onCode;    // Discrete power-on IR code
    uint32_t   offCode;   // Discrete power-off IR code
    bool       visible;   // Whether this profile appears in the button cycle
};

// ---------------------------------------------------------------------------
// Settings struct — device-wide configuration
// ---------------------------------------------------------------------------
struct Settings {
    int  activeProfile;    // Index of the currently selected profile
    bool displayAlwaysOn;  // Keep OLED on during normal (non-wireless) operation
};

// ---------------------------------------------------------------------------
// Profiles namespace — public API
// ---------------------------------------------------------------------------
namespace Profiles {

    // Initialise LittleFS and load profiles and settings into memory.
    // Writes /profiles.json and /settings.json from hardcoded defaults
    // on first boot if the files do not already exist.
    // Returns false if LittleFS fails to mount — firmware should log and
    // continue, IR commands will not work without a loaded profile.
    bool begin();

    // Read access to loaded data
    const std::vector<Profile>& getAll();
    const Profile&               getActive();
    const Settings&              getSettings();

    // Write access to settings — call saveSettings() to persist changes
    Settings& getMutableSettings();

    // Replace the in-memory profile list (e.g. from a web UI POST).
    // Clamps the active profile index if it exceeds the new list size.
    // Does not persist — call saveProfiles()/saveSettings() after.
    void replaceAll(std::vector<Profile> newProfiles);

    // Returns the index of the next visible profile after the active one.
    // Wraps around the list. Returns the current index if no other visible
    // profiles exist (single-profile or all others hidden).
    int nextVisibleIndex();

    // Persist current in-memory state to LittleFS
    void saveProfiles();
    void saveSettings();

    // Overwrite /profiles.json and /settings.json from hardcoded defaults,
    // then reload into memory. Called by button hold and web UI factory reset.
    void factoryReset();

    // Protocol string conversion — used when reading/writing JSON
    IrProtocol protocolFromString(const String& s);
    String     protocolToString(IrProtocol p);

} // namespace Profiles
