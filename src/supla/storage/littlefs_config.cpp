// SPDX-FileCopyrightText: AC SOFTWARE SP. Z O.O.
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SUPLA_EXCLUDE_LITTLEFS_CONFIG

#if !defined(ARDUINO_ARCH_AVR)
// don't compile it on Arduino Mega

#include "littlefs_config.h"

#include <LittleFS.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <supla/log_wrapper.h>
#include <supla/storage/key_value.h>

// Weak hooks fulfilled by z2s_little_fs.cpp so Supla and Z2S share saveMutex
// and respect Z2S_endLittleFs(true) shutdown state, while remaining link-safe
// if compiled in a non-Z2S sketch.
bool Z2S_lockFs(uint32_t timeout_ms) __attribute__((weak));
void Z2S_unlockFs() __attribute__((weak));

namespace Supla {
const char ConfigFileName[]       = "/supla-dev.cfg";
const char BackupConfigFileName[] = "/supla-dev.cfg.bak";
const char CustomCAFileName[]     = "/custom_ca.pem";
const char MqttCAFileName[]       = "/mqtt_ca.pem";
};  // namespace Supla

namespace {

static constexpr size_t   SUPLA_FS_PATH_MAX         = 96;
static constexpr uint32_t SUPLA_FS_MUTEX_TIMEOUT_MS = 2000;

// Allow auto-format at most once during early boot init() on a blank chip.
static bool s_supla_allow_initial_format = true;

class SuplaFsGuard {
 public:
  explicit SuplaFsGuard(uint32_t timeout_ms = SUPLA_FS_MUTEX_TIMEOUT_MS) {
    if (Z2S_lockFs) {
      locked_     = Z2S_lockFs(timeout_ms);
      used_mutex_ = locked_;
    } else {
      locked_     = true;
      used_mutex_ = false;
    }
  }

  ~SuplaFsGuard() {
    if (used_mutex_ && Z2S_unlockFs) {
      Z2S_unlockFs();
    }
  }

  bool locked() const { return locked_; }

  SuplaFsGuard(const SuplaFsGuard&)            = delete;
  SuplaFsGuard& operator=(const SuplaFsGuard&) = delete;

 private:
  bool locked_     = false;
  bool used_mutex_ = false;
};

static bool atomicRenameLocked(const char* srcPath, const char* dstPath) {
  if (!LittleFS.exists(srcPath)) {
    return false;
  }

  if (LittleFS.rename(srcPath, dstPath)) {
    return true;
  }

  if (LittleFS.exists(dstPath)) {
    if (!LittleFS.remove(dstPath)) {
      return false;
    }
    return LittleFS.rename(srcPath, dstPath);
  }

  return false;
}

static bool atomicWriteFileLocked(const char* dstPath,
                                  const uint8_t* data,
                                  size_t dataSize,
                                  const char* backupPath = nullptr) {
  if (!dstPath || (dataSize > 0 && !data)) {
    return false;
  }

  char tmpPath[SUPLA_FS_PATH_MAX] = {};
  int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", dstPath);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(tmpPath)) {
    SUPLA_LOG_ERROR("LittleFsConfig: temp path truncated for \"%s\"", dstPath);
    return false;
  }

  File tmp = LittleFS.open(tmpPath, "w");
  if (!tmp) {
    SUPLA_LOG_ERROR("LittleFsConfig: failed to open temp file \"%s\"", tmpPath);
    return false;
  }

  size_t written = 0;
  if (dataSize > 0) {
    written = tmp.write(data, dataSize);
  }
  tmp.flush();
  tmp.close();

  if (written != dataSize) {
    SUPLA_LOG_ERROR("LittleFsConfig: short write for \"%s\" (%u/%u B)",
                    dstPath,
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(dataSize));
    LittleFS.remove(tmpPath);
    return false;
  }

  if (backupPath && LittleFS.exists(dstPath)) {
    if (!atomicRenameLocked(dstPath, backupPath)) {
      SUPLA_LOG_WARNING("LittleFsConfig: failed to rotate \"%s\" to \"%s\"",
                        dstPath,
                        backupPath);
    }
  }

  if (!atomicRenameLocked(tmpPath, dstPath)) {
    SUPLA_LOG_ERROR("LittleFsConfig: failed to promote \"%s\" to \"%s\"",
                    tmpPath,
                    dstPath);
    LittleFS.remove(tmpPath);
    return false;
  }

  return true;
}

}  // namespace

#define BIG_BLOG_SIZE_TO_BE_STORED_IN_FILE 32

Supla::LittleFsConfig::LittleFsConfig(int configMaxSize)
    : configMaxSize(configMaxSize) {
}

Supla::LittleFsConfig::~LittleFsConfig() {
}

bool Supla::LittleFsConfig::init() {
  SUPLA_LOG_WARNING(
      "LittleFsConfig: config stored without encryption (LittleFS). "
      "Device is not protected against physical access.");
  if (first) {
    SUPLA_LOG_WARNING(
        "LittleFsConfig: init called on non empty database. Aborting");
    // init can be done only on empty storage
    initResult = false;
    return false;
  }

  SuplaFsGuard guard;
  if (!guard.locked()) {
    SUPLA_LOG_ERROR("LittleFsConfig: mutex timeout in init()");
    initResult = false;
    return false;
  }

  if (!initLittleFs()) {
    initResult = false;
    return false;
  }

  if (buf == nullptr) {
    buf = new uint8_t[configMaxSize];
  }

  auto files = {ConfigFileName, BackupConfigFileName};
  initResult = false;

  for (auto file : files) {
    if (LittleFS.exists(file)) {
      File cfg = LittleFS.open(file, "r");
      if (!cfg) {
        SUPLA_LOG_ERROR("LittleFsConfig: failed to open \"%s\"", file);
        continue;
      }

      int fileSize = cfg.size();

      SUPLA_LOG_DEBUG("LittleFsConfig: file \"%s\" size %d", file, fileSize);
      if (fileSize > configMaxSize) {
        SUPLA_LOG_ERROR("LittleFsConfig: config file is too big");
        cfg.close();
        continue;
      }

      if (buf == nullptr) {
        SUPLA_LOG_ERROR("LittleFsConfig: failed to allocate memory");
        cfg.close();
        continue;
      }

      memset(buf, 0, configMaxSize);
      int bytesRead = cfg.read(buf, fileSize);
      cfg.close();
      if (bytesRead != fileSize) {
        SUPLA_LOG_DEBUG("LittleFsConfig: read bytes %d, while file is %d bytes",
                        bytesRead,
                        fileSize);
        continue;
      }

      SUPLA_LOG_DEBUG("LittleFsConfig: initializing storage from file...");
      initResult = initFromMemory(buf, fileSize);
      SUPLA_LOG_DEBUG("LittleFsConfig: init result %s",
                      initResult ? "success" : "failure");

      if (!initResult) {
        continue;
      } else {
        break;
      }
    } else {
      SUPLA_LOG_DEBUG("LittleFsConfig:: config file \"%s\" missing", file);
    }
  }

  return initResult;
}

void Supla::LittleFsConfig::commit() {
  if (buf == nullptr) {
    SUPLA_LOG_ERROR("LittleFsConfig: failed to allocate memory");
    return;
  }

  memset(buf, 0, configMaxSize);
  size_t dataSize = serializeToMemory(buf, configMaxSize);

  SuplaFsGuard guard;
  if (!guard.locked()) {
    SUPLA_LOG_ERROR("LittleFsConfig: mutex timeout in commit()");
    return;
  }

  if (!initLittleFs()) {
    return;
  }

  SUPLA_LOG_DEBUG("LittleFsConfig: committing %u bytes atomically",
                  static_cast<unsigned>(dataSize));

  // Write to .tmp first, then rotate previous primary -> .bak, then promote .tmp -> primary.
  if (!atomicWriteFileLocked(
          ConfigFileName, buf, dataSize, BackupConfigFileName)) {
    return;
  }

  // Ensure backup file also exists after the very first commit on a fresh chip.
  if (!LittleFS.exists(BackupConfigFileName)) {
    atomicWriteFileLocked(BackupConfigFileName, buf, dataSize, nullptr);
  }
}

bool Supla::LittleFsConfig::getCustomCA(char* customCA, int maxSize) {
  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return false;
  }

  if (LittleFS.exists(CustomCAFileName)) {
    File file = LittleFS.open(CustomCAFileName, "r");
    if (!file) {
      SUPLA_LOG_ERROR("LittleFsConfig: failed to open custom CA file");
      return false;
    }

    int fileSize = file.size();

    if (fileSize > maxSize) {
      SUPLA_LOG_ERROR("LittleFsConfig: custom CA file is too big");
      file.close();
      return false;
    }

    int bytesRead = file.read(reinterpret_cast<uint8_t*>(customCA), fileSize);
    file.close();

    if (bytesRead != fileSize) {
      SUPLA_LOG_DEBUG("LittleFsConfig: read bytes %d, while file is %d bytes",
                      bytesRead,
                      fileSize);
      return false;
    }

    return true;
  } else {
    SUPLA_LOG_DEBUG("LittleFsConfig:: custom ca file missing");
  }

  return true;
}

int Supla::LittleFsConfig::getCustomCASize() {
  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return 0;
  }

  if (LittleFS.exists(CustomCAFileName)) {
    File file = LittleFS.open(CustomCAFileName, "r");
    if (!file) {
      SUPLA_LOG_ERROR("LittleFsConfig: failed to open custom CA file");
      return 0;
    }

    int fileSize = file.size();
    file.close();
    return fileSize;
  }

  return 0;
}

bool Supla::LittleFsConfig::setCustomCA(const char* customCA) {
  if (customCA == nullptr) {
    return false;
  }

  size_t dataSize = strlen(customCA);

  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return false;
  }

  return atomicWriteFileLocked(
      CustomCAFileName, reinterpret_cast<const uint8_t*>(customCA), dataSize);
}

bool Supla::LittleFsConfig::getMqttCA(char* mqttCA, int maxSize) {
  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return false;
  }

  if (!LittleFS.exists(MqttCAFileName)) {
    return false;
  }

  File file = LittleFS.open(MqttCAFileName, "r");
  if (!file) {
    SUPLA_LOG_ERROR("LittleFsConfig: failed to open MQTT CA file");
    return false;
  }

  int fileSize = file.size();
  if (fileSize >= maxSize) {
    SUPLA_LOG_ERROR("LittleFsConfig: MQTT CA file is too big");
    file.close();
    return false;
  }

  int bytesRead = file.read(reinterpret_cast<uint8_t*>(mqttCA), fileSize);
  file.close();

  if (bytesRead != fileSize) {
    return false;
  }
  mqttCA[fileSize] = '\0';
  return true;
}

int Supla::LittleFsConfig::getMqttCASize() {
  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return 0;
  }

  int fileSize = 0;
  if (LittleFS.exists(MqttCAFileName)) {
    File file = LittleFS.open(MqttCAFileName, "r");
    if (file) {
      fileSize = file.size();
      file.close();
    } else {
      SUPLA_LOG_ERROR("LittleFsConfig: failed to open MQTT CA file");
    }
  }

  return fileSize;
}

bool Supla::LittleFsConfig::setMqttCA(const char* mqttCA) {
  if (mqttCA == nullptr) {
    return false;
  }

  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return false;
  }

  size_t dataSize = strlen(mqttCA);
  return atomicWriteFileLocked(
      MqttCAFileName, reinterpret_cast<const uint8_t*>(mqttCA), dataSize);
}

bool Supla::LittleFsConfig::initLittleFs() {
  bool result = LittleFS.begin(false);
  if (!result) {
    if (s_supla_allow_initial_format) {
      SUPLA_LOG_WARNING(
          "LittleFsConfig: initial mount failed - formatting partition");
      if (LittleFS.format()) {
        result = LittleFS.begin(false);
      }
      if (!result) {
        SUPLA_LOG_ERROR(
            "LittleFsConfig: failed to mount and to format partition");
      }
    } else {
      SUPLA_LOG_ERROR(
          "LittleFsConfig: mount failed (runtime auto-format disabled)");
    }
  }

  s_supla_allow_initial_format = false;
  return result;
}

void Supla::LittleFsConfig::removeAll() {
  SUPLA_LOG_DEBUG("LittleFsConfig remove all called");

  {
    SuplaFsGuard guard;
    if (guard.locked() && initLittleFs()) {
      LittleFS.remove(CustomCAFileName);
      LittleFS.remove(MqttCAFileName);

      File suplaDir = LittleFS.open("/supla", "r");
      if (suplaDir && suplaDir.isDirectory()) {
        File file = suplaDir.openNextFile();
        while (file) {
          bool isDir = file.isDirectory();
          char path[200] = {};
          if (!isDir) {
            SUPLA_LOG_DEBUG("LittleFsConfig: removing file /supla/%s",
                            file.name());
            snprintf(path, sizeof(path), "/supla/%s", file.name());
          }
          file.close();

          if (!isDir && path[0] != '\0') {
            if (!LittleFS.remove(path)) {
              SUPLA_LOG_ERROR("LittleFsConfig: failed to remove file");
            }
          }
          file = suplaDir.openNextFile();
        }
        suplaDir.close();
      } else {
        if (suplaDir) {
          suplaDir.close();
        }
        SUPLA_LOG_DEBUG("LittleFsConfig: failed to open supla directory");
      }
    }
  }

  Supla::KeyValue::removeAll();
}

bool Supla::LittleFsConfig::setBlob(const char* key,
                                    const char* value,
                                    size_t blobSize) {
  if (key == nullptr || value == nullptr) {
    return false;
  }

  char filename[SUPLA_FS_PATH_MAX] = {};
  int n = snprintf(filename, sizeof(filename), "/supla/%s", key);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(filename)) {
    SUPLA_LOG_ERROR("LittleFsConfig: blob key path too long \"%s\"", key);
    return false;
  }

  if (blobSize < BIG_BLOG_SIZE_TO_BE_STORED_IN_FILE) {
    bool result = Supla::KeyValue::setBlob(key, value, blobSize);
    if (result) {
      SuplaFsGuard guard;
      if (guard.locked() && initLittleFs() && LittleFS.exists(filename)) {
        LittleFS.remove(filename);
      }
    }
    return result;
  }

  Supla::KeyValue::eraseKey(key);
  SUPLA_LOG_DEBUG("LittleFS: writing file %s", key);

  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return false;
  }

  if (!LittleFS.exists("/supla")) {
    LittleFS.mkdir("/supla");
  }

  return atomicWriteFileLocked(
      filename, reinterpret_cast<const uint8_t*>(value), blobSize);
}

bool Supla::LittleFsConfig::getBlob(const char* key,
                                    char* value,
                                    size_t blobSize) {
  if (key == nullptr || value == nullptr) {
    return false;
  }

  int keyValueBlobSize = Supla::KeyValue::getBlobSize(key);
  if (keyValueBlobSize >= 0) {
    if (static_cast<size_t>(keyValueBlobSize) > blobSize) {
      return false;
    }
    return Supla::KeyValue::getBlob(
        key, value, static_cast<size_t>(keyValueBlobSize));
  }

  char filename[SUPLA_FS_PATH_MAX] = {};
  int n = snprintf(filename, sizeof(filename), "/supla/%s", key);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(filename)) {
    return false;
  }

  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return false;
  }

  if (!LittleFS.exists(filename)) {
    return false;
  }

  File file = LittleFS.open(filename, "r");
  if (!file) {
    SUPLA_LOG_ERROR("LittleFsConfig: failed to open blob file \"%s\" for read",
                    key);
    return false;
  }

  size_t fileSize = file.size();
  if (fileSize > blobSize) {
    SUPLA_LOG_ERROR("LittleFsConfig: blob file does not fit in buffer");
    file.close();
    return false;
  }

  int bytesRead = file.read(reinterpret_cast<uint8_t*>(value), fileSize);
  file.close();
  return (bytesRead < 0) ? false : static_cast<size_t>(bytesRead) == fileSize;
}

int Supla::LittleFsConfig::getBlobSize(const char* key) {
  if (key == nullptr) {
    return -1;
  }

  int keyValueBlobSize = Supla::KeyValue::getBlobSize(key);
  if (keyValueBlobSize >= 0) {
    return keyValueBlobSize;
  }

  char filename[SUPLA_FS_PATH_MAX] = {};
  int n = snprintf(filename, sizeof(filename), "/supla/%s", key);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(filename)) {
    return -1;
  }

  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return -1;
  }

  if (!LittleFS.exists(filename)) {
    return -1;
  }

  File file = LittleFS.open(filename, "r");
  if (!file) {
    SUPLA_LOG_DEBUG(
        "LittleFsConfig: failed to open blob file \"%s\", blob not found", key);
    return -1;
  }

  int fileSize = file.size();
  file.close();
  return fileSize;
}

bool Supla::LittleFsConfig::eraseKey(const char* key) {
  if (key == nullptr) {
    return false;
  }

  bool removed = Supla::KeyValue::eraseKey(key);

  char filename[SUPLA_FS_PATH_MAX] = {};
  int n = snprintf(filename, sizeof(filename), "/supla/%s", key);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(filename)) {
    return removed;
  }

  SuplaFsGuard guard;
  if (!guard.locked() || !initLittleFs()) {
    return removed;
  }

  if (LittleFS.exists(filename) && LittleFS.remove(filename)) {
    removed = true;
  }

  return removed;
}

#endif  // !defined(ARDUINO_ARCH_AVR)
#endif  // SUPLA_EXCLUDE_LITTLEFS_CONFIG