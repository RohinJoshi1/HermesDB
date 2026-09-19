#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace hermesdb::bench {

class Histogram {
 public:
  void add(std::uint64_t nanos) {
    ++count_;
    total_ += nanos;
    if (nanos > max_) max_ = nanos;
    const auto bucket = bucket_for(nanos);
    if (bucket >= counts_.size()) counts_.resize(bucket + 1);
    ++counts_[bucket];
  }

  [[nodiscard]] std::uint64_t count() const { return count_; }
  [[nodiscard]] std::uint64_t max_ns() const { return max_; }
  [[nodiscard]] double avg_ns() const {
    return count_ == 0 ? 0.0 : static_cast<double>(total_) / static_cast<double>(count_);
  }
  void merge(const Histogram& other) {
    if (other.counts_.size() > counts_.size()) {
      counts_.resize(other.counts_.size());
    }
    for (std::size_t i = 0; i < other.counts_.size(); ++i) {
      counts_[i] += other.counts_[i];
    }
    count_ += other.count_;
    total_ += other.total_;
    if (other.max_ > max_) max_ = other.max_;
  }
  [[nodiscard]] std::uint64_t percentile(double p) const {
    if (count_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(
        std::ceil(p * static_cast<double>(count_)));
    std::uint64_t seen = 0;
    for (std::size_t bucket = 0; bucket < counts_.size(); ++bucket) {
      seen += counts_[bucket];
      if (seen >= target) return bucket_ns(bucket);
    }
    return max_;
  }

 private:
  static std::size_t bucket_for(std::uint64_t nanos) {
    if (nanos <= 1) return 0;
    std::size_t bucket = 0;
    auto value = nanos;
    while (value > 1) {
      value >>= 1;
      ++bucket;
    }
    return bucket;
  }
  static std::uint64_t bucket_ns(std::size_t bucket) {
    return bucket == 0 ? 1 : (std::uint64_t{1} << bucket);
  }

  std::vector<std::uint64_t> counts_;
  std::uint64_t count_{};
  std::uint64_t total_{};
  std::uint64_t max_{};
};

class Zipf {
 public:
  Zipf(std::uint64_t items, double theta, std::uint64_t seed)
      : items_(std::max<std::uint64_t>(items, 1)), theta_(theta), rng_(seed) {
    zeta_n_ = zeta(items_, theta_);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(items_), 1.0 - theta_)) /
           (1.0 - zeta(2, theta_) / zeta_n_);
  }

  std::uint64_t next() {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double u = unit(rng_);
    const double uz = u * zeta_n_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
    const auto rank = static_cast<std::uint64_t>(
        static_cast<double>(items_) *
        std::pow(eta_ * u - eta_ + 1.0, 1.0 / (1.0 - theta_)));
    return std::min(rank, items_ - 1);
  }

 private:
  static double zeta(std::uint64_t n, double theta) {
    double sum = 0.0;
    for (std::uint64_t i = 1; i <= n; ++i) {
      sum += std::pow(1.0 / static_cast<double>(i), theta);
    }
    return sum;
  }

  std::uint64_t items_;
  double theta_;
  double zeta_n_{};
  double eta_{};
  std::mt19937_64 rng_;
};

enum class Distribution { uniform, zipfian, latest };

inline std::uint64_t scramble(std::uint64_t value) {
  value ^= value >> 33U;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33U;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33U;
  return value;
}

class KeyChooser {
 public:
  KeyChooser(Distribution distribution, std::uint64_t items, std::uint64_t seed)
      : distribution_(distribution), items_(std::max<std::uint64_t>(items, 1)),
        rng_(seed), zipf_(items_, 0.99, seed ^ 0x9e3779b97f4a7c15ULL) {}

  std::uint64_t next() {
    std::uint64_t raw = 0;
    switch (distribution_) {
      case Distribution::uniform: {
        std::uniform_int_distribution<std::uint64_t> dist(0, items_ - 1);
        raw = dist(rng_);
        break;
      }
      case Distribution::zipfian:
        raw = scramble(zipf_.next()) % items_;
        break;
      case Distribution::latest: {
        std::geometric_distribution<std::uint64_t> geo(0.25);
        const auto offset = std::min(geo(rng_), items_ - 1);
        raw = items_ - 1 - offset;
        break;
      }
    }
    return raw;
  }

 private:
  Distribution distribution_;
  std::uint64_t items_;
  std::mt19937_64 rng_;
  Zipf zipf_;
};

inline std::string make_key(std::uint64_t id) {
  std::string key(16, '0');
  for (int i = 15; i >= 0; --i) {
    key[static_cast<std::size_t>(i)] =
        static_cast<char>('0' + static_cast<int>(id % 10));
    id /= 10;
  }
  return key;
}

inline std::string make_value(std::uint64_t id, std::size_t size) {
  std::string value(size, 'x');
  for (std::size_t i = 0; i < size; ++i) {
    value[i] = static_cast<char>('a' + static_cast<int>((id + i) % 26));
  }
  return value;
}

inline auto now() { return std::chrono::steady_clock::now(); }

}  // namespace hermesdb::bench
