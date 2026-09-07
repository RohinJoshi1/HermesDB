#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tiny_lsm::persistence {

using Bytes = std::vector<std::byte>;

// IEEE CRC-32 (the CRC used by crc32fast and zlib).
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view data) noexcept;

struct WalRecord {
  Bytes key;
  Bytes value;

  friend bool operator==(const WalRecord&, const WalRecord&) = default;
};

struct MvccWalRecord {
  Bytes key;
  std::uint64_t timestamp{};
  Bytes value;

  friend bool operator==(const MvccWalRecord&, const MvccWalRecord&) = default;
};

// Base mini-lsm WAL format:
//   u16be key-size | key | u16be value-size | value | u32be crc32(preceding fields)
class Wal {
 public:
  struct Recovery {
    std::unique_ptr<Wal> wal;
    std::vector<WalRecord> records;
    bool truncated_tail{};
  };

  static std::unique_ptr<Wal> create(const std::filesystem::path& path);
  static Recovery recover(const std::filesystem::path& path);

  ~Wal();
  Wal(Wal&&) noexcept;
  Wal& operator=(Wal&&) noexcept;
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  void append(std::span<const std::byte> key, std::span<const std::byte> value);
  void append(std::string_view key, std::string_view value);
  void sync();

 private:
  struct Impl;
  explicit Wal(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// MVCC WAL frames make a whole write batch atomic:
//   u32be payload-size | repeated(u16be key-size | key | u64be ts |
//   u16be value-size | value) | u32be crc32(payload)
class MvccWal {
 public:
  struct Recovery {
    std::unique_ptr<MvccWal> wal;
    std::vector<std::vector<MvccWalRecord>> batches;
    bool truncated_tail{};

    [[nodiscard]] std::vector<MvccWalRecord> records() const;
  };

  static std::unique_ptr<MvccWal> create(const std::filesystem::path& path);
  static Recovery recover(const std::filesystem::path& path);

  ~MvccWal();
  MvccWal(MvccWal&&) noexcept;
  MvccWal& operator=(MvccWal&&) noexcept;
  MvccWal(const MvccWal&) = delete;
  MvccWal& operator=(const MvccWal&) = delete;

  void append_batch(std::span<const MvccWalRecord> records);
  void append(const MvccWalRecord& record);
  void sync();

 private:
  struct Impl;
  explicit MvccWal(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// MANIFEST framing matches mini-lsm:
//   u64be payload-size | payload | u32be crc32(payload)
// Payloads are deliberately raw. In particular, the exact serde_json bytes
// produced by the Rust implementation can be appended and recovered unchanged.
struct ManifestRecord {
  Bytes payload;

  friend bool operator==(const ManifestRecord&, const ManifestRecord&) = default;
};

enum class ManifestRecordKind : std::uint8_t {
  flush = 1,
  new_memtable = 2,
  compaction = 3,
};

// Dependency-free, versioned payload representation for C++ users. Compaction
// tasks remain opaque bytes so engines can evolve their own task schema.
struct InternalManifestRecord {
  ManifestRecordKind kind{};
  std::uint64_t object_id{};
  Bytes compaction_task;
  std::vector<std::uint64_t> output_ids;

  friend bool operator==(const InternalManifestRecord&,
                         const InternalManifestRecord&) = default;
};

[[nodiscard]] Bytes encode_manifest_payload(const InternalManifestRecord& record);
[[nodiscard]] InternalManifestRecord decode_manifest_payload(
    std::span<const std::byte> payload);

class Manifest {
 public:
  struct Recovery {
    std::unique_ptr<Manifest> manifest;
    std::vector<ManifestRecord> records;
    bool truncated_tail{};
  };

  static std::unique_ptr<Manifest> create(const std::filesystem::path& path);
  static Recovery recover(const std::filesystem::path& path);

  ~Manifest();
  Manifest(Manifest&&) noexcept;
  Manifest& operator=(Manifest&&) noexcept;
  Manifest(const Manifest&) = delete;
  Manifest& operator=(const Manifest&) = delete;

  // append() synchronizes before returning, as required for MANIFEST ordering.
  void append(std::span<const std::byte> payload);
  void append(const ManifestRecord& record);
  void append(const InternalManifestRecord& record);
  void sync();

 private:
  struct Impl;
  explicit Manifest(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tiny_lsm::persistence
