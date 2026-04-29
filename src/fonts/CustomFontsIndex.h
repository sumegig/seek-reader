#pragma once

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>

// CustomFontsIndex
// Manages /.crosspoint/fonts/custom_fonts.json
//
// Goals:
// - Provide "slot exists + display name" without scanning directories
// - Track which styles exist per slot (bitmask)
// - Be easy to use from both Settings UI and Web API
//
// JSON format (v1):
// {
//   "version": 1,
//   "slots": [
//     { "id": 1, "active": true, "name": "MyFont", "styleMask": 15 },
//     { "id": 2, "active": false, "name": "", "styleMask": 0 },
//     { "id": 3, "active": false, "name": "", "styleMask": 0 }
//   ]
// }
//
// styleMask bits (match your 4-style taxonomy):
//   bit0 = regular
//   bit1 = bold
//   bit2 = italic
//   bit3 = bolditalic
//
class CustomFontsIndex {
 public:
  static constexpr const char* kDir = "/.crosspoint/fonts";
  static constexpr const char* kPath = "/.crosspoint/fonts/custom_fonts.json";
  static constexpr uint8_t kVersion = 1;
  static constexpr uint8_t kSlotCount = 3;

  enum StyleBit : uint8_t {
    STYLE_REGULAR     = 1u << 0,
    STYLE_BOLD        = 1u << 1,
    STYLE_ITALIC      = 1u << 2,
    STYLE_BOLD_ITALIC = 1u << 3,
  };

  struct Slot {
    uint8_t id = 0;           // 1..3
    bool active = false;      // "exists"
    uint8_t styleMask = 0;    // bitmask of StyleBit
    char name[32] = "";       // display name (user-provided)
  };

  CustomFontsIndex() { resetToDefaults(); }

  // Load index from SD. If missing/corrupt, creates defaults and writes them.
  bool loadOrCreate() {
    Storage.mkdir(kDir);

    if (!Storage.exists(kPath)) {
      LOG_DBG("CFI", "Index missing, creating: %s", kPath);
      resetToDefaults();
      return save();
    }

    const String json = Storage.readFile(kPath);
    if (json.isEmpty()) {
      LOG_ERR("CFI", "Index file empty/unreadable, recreating: %s", kPath);
      resetToDefaults();
      return save();
    }

    JsonDocument doc;
    const auto err = deserializeJson(doc, json);
    if (err) {
      LOG_ERR("CFI", "Index JSON parse failed (%s), recreating", err.c_str());
      resetToDefaults();
      return save();
    }

    const uint8_t version = doc["version"] | 0;
    if (version != kVersion) {
      LOG_ERR("CFI", "Index version mismatch (%u != %u), recreating", version, kVersion);
      resetToDefaults();
      return save();
    }

    const JsonArray slotsArr = doc["slots"].as<JsonArray>();
    if (slotsArr.isNull() || slotsArr.size() != kSlotCount) {
      LOG_ERR("CFI", "Index slots missing/invalid, recreating");
      resetToDefaults();
      return save();
    }

    // Load slots
    for (uint8_t i = 0; i < kSlotCount; i++) {
      const JsonObject o = slotsArr[i].as<JsonObject>();
      const int id = o["id"] | int(i + 1);
      slots_[i].id = clampSlotId(id);

      slots_[i].active = o["active"] | false;

      const uint32_t sm = o["styleMask"] | 0;
      slots_[i].styleMask = static_cast<uint8_t>(sm & 0x0F);

      const char* nm = o["name"] | "";
      setName(slots_[i], nm);
    }

    // Ensure IDs are 1..3 in order (avoid weird JSON edits)
    normalizeIds();

    return true;
  }

  bool save() const {
    Storage.mkdir(kDir);

    JsonDocument doc;
    doc["version"] = kVersion;
    JsonArray slotsArr = doc["slots"].to<JsonArray>();

    for (uint8_t i = 0; i < kSlotCount; i++) {
      JsonObject o = slotsArr.add<JsonObject>();
      o["id"] = slots_[i].id;
      o["active"] = slots_[i].active;
      o["name"] = slots_[i].name;
      o["styleMask"] = slots_[i].styleMask;
    }

    String out;
    serializeJson(doc, out);

    // HalStorage has readFile; write API varies. Use openFileForWrite for consistency with your code style.
    FsFile f;
    if (!Storage.openFileForWrite("CFI", kPath, f)) {
      LOG_ERR("CFI", "Failed to open for write: %s", kPath);
      return false;
    }
    const size_t wrote = f.write(reinterpret_cast<const uint8_t*>(out.c_str()), out.length());
    f.close();

    if (wrote != out.length()) {
      LOG_ERR("CFI", "Short write to %s (%u/%u)", kPath, (unsigned)wrote, (unsigned)out.length());
      return false;
    }

    return true;
  }

  // ---- Slot queries ----

  const Slot& getSlot(uint8_t slotId) const {
    const uint8_t idx = slotIndex(slotId);
    return slots_[idx];
  }

  bool slotExists(uint8_t slotId) const {
    const Slot& s = getSlot(slotId);
    return s.active && s.name[0] != '\0';
  }

  const char* slotName(uint8_t slotId) const { return getSlot(slotId).name; }

  uint8_t slotStyleMask(uint8_t slotId) const { return getSlot(slotId).styleMask; }

  // "Slot1: MyFont" (empty if not existing)
  String slotDisplayLabel(uint8_t slotId) const {
    if (!slotExists(slotId)) return "";
    const Slot& s = getSlot(slotId);
    return String("Slot") + String(s.id) + ": " + String(s.name);
  }

  // ---- Slot updates ----

  // Set/activate a slot (used after successful upload batch)
  bool setSlot(uint8_t slotId, const char* name, uint8_t styleMask, bool active = true) {
    Slot& s = slots_[slotIndex(slotId)];
    s.active = active;
    s.styleMask = (styleMask & 0x0F);
    setName(s, name ? name : "");
    return true;
  }

  // Clear slot metadata (you may also delete the .bin files separately)
  bool clearSlot(uint8_t slotId) {
    Slot& s = slots_[slotIndex(slotId)];
    s.active = false;
    s.styleMask = 0;
    s.name[0] = '\0';
    return true;
  }

  void resetToDefaults() {
    for (uint8_t i = 0; i < kSlotCount; i++) {
      slots_[i] = {};
      slots_[i].id = i + 1;
      slots_[i].active = false;
      slots_[i].styleMask = 0;
      slots_[i].name[0] = '\0';
    }
  }

 private:
  Slot slots_[kSlotCount];

  static uint8_t clampSlotId(int id) {
    if (id < 1) return 1;
    if (id > kSlotCount) return kSlotCount;
    return static_cast<uint8_t>(id);
  }

  static uint8_t slotIndex(uint8_t slotId) {
    if (slotId < 1) return 0;
    if (slotId > kSlotCount) return kSlotCount - 1;
    return slotId - 1;
  }

  static void setName(Slot& s, const char* name) {
    // Trim leading/trailing spaces very simply (avoid heavy string ops)
    while (*name == ' ' || *name == '\t' || *name == '\n' || *name == '\r') name++;

    // Copy with truncation
    std::strncpy(s.name, name, sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = '\0';

    // Trim trailing whitespace
    for (int i = (int)std::strlen(s.name) - 1; i >= 0; i--) {
      if (s.name[i] == ' ' || s.name[i] == '\t' || s.name[i] == '\n' || s.name[i] == '\r') {
        s.name[i] = '\0';
      } else {
        break;
      }
    }
  }

  void normalizeIds() {
    for (uint8_t i = 0; i < kSlotCount; i++) {
      slots_[i].id = i + 1;
    }
  }
};