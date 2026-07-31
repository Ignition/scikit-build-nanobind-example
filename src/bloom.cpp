// A Bloom filter in C++20, bound with nanobind.
//
// The filter itself is deliberately plain: a packed bit array, two hashes, and
// the standard sizing formulas. The interesting part of this project is the
// binding and build machinery around it (see README.md).

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/tuple.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// Final mixing step of splitmix64: cheap, and gives good avalanche on the
// fairly weak FNV output below.
constexpr std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

constexpr std::uint64_t fnv1a(std::span<const std::byte> data, std::uint64_t basis) noexcept {
    std::uint64_t hash = basis;
    for (std::byte b : data) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 0x100000001b3ULL;  // FNV-1a 64-bit prime
    }
    return hash;
}

// Shortest round-trippable representation, so repr(0.01) reads as "0.01".
std::string format_double(double value) {
    char buffer[32];
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return ec == std::errc{} ? std::string(buffer, end) : std::string("?");
}

class BloomFilter {
public:
    BloomFilter(std::size_t capacity, double error_rate)
        : capacity_(capacity), error_rate_(error_rate) {
        if (capacity == 0) {
            throw std::invalid_argument("capacity must be positive");
        }
        if (!(error_rate > 0.0 && error_rate < 1.0)) {
            throw std::invalid_argument("error_rate must lie strictly between 0 and 1");
        }

        // m = -n ln(p) / (ln 2)^2, k = (m/n) ln 2 — the standard optimum.
        static constexpr double kLn2 = 0.693147180559945309417;
        const auto n = static_cast<double>(capacity);
        const double bits = -n * std::log(error_rate) / (kLn2 * kLn2);

        num_bits_ = static_cast<std::size_t>(std::ceil(bits));
        num_hashes_ = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::llround(kLn2 * bits / n)));
        words_.assign((num_bits_ + 63) / 64, 0);
    }

    // Used only when unpickling, where the bit array already exists.
    BloomFilter(std::size_t capacity, double error_rate, std::size_t num_bits,
                std::size_t num_hashes, std::size_t count, std::vector<std::uint64_t> words)
        : capacity_(capacity), error_rate_(error_rate), num_bits_(num_bits),
          num_hashes_(num_hashes), count_(count), words_(std::move(words)) {}

    void add(std::span<const std::byte> key) noexcept {
        // Plain writes, no atomics: a filter is owned by one thread by contract.
        // See README — making this atomic would tax the only hot path here.
        auto [h1, h2] = hashes(key);
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            const std::size_t bit = static_cast<std::size_t>((h1 + i * h2) % num_bits_);
            words_[bit / 64] |= std::uint64_t{1} << (bit % 64);
        }
        ++count_;
    }

    bool contains(std::span<const std::byte> key) const noexcept {
        auto [h1, h2] = hashes(key);
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            const std::size_t bit = static_cast<std::size_t>((h1 + i * h2) % num_bits_);
            if ((words_[bit / 64] & (std::uint64_t{1} << (bit % 64))) == 0) {
                return false;  // a clear bit proves absence — never a false negative
            }
        }
        return true;
    }

    BloomFilter union_with(const BloomFilter& other) const {
        if (num_bits_ != other.num_bits_ || num_hashes_ != other.num_hashes_) {
            throw std::invalid_argument(
                "cannot union filters with different parameters: "
                "both must share the same capacity and error_rate");
        }
        BloomFilter merged = *this;
        for (std::size_t i = 0; i < words_.size(); ++i) {
            merged.words_[i] |= other.words_[i];
        }
        merged.count_ = count_ + other.count_;
        return merged;
    }

    // Deliberately no operator== / __eq__. Equality is meaningless for a Bloom
    // filter: identical bit arrays do not imply identical contents, and differing
    // ones do not imply differing contents. Exposing it would invite a wrong
    // conclusion. Tests that need a structural comparison use __getstate__.

    std::string repr() const {
        return "BloomFilter(capacity=" + std::to_string(capacity_) +
               ", error_rate=" + format_double(error_rate_) +
               ", items=" + std::to_string(count_) + ")";
    }

    std::size_t capacity() const noexcept { return capacity_; }
    double error_rate() const noexcept { return error_rate_; }
    std::size_t num_bits() const noexcept { return num_bits_; }
    std::size_t num_hashes() const noexcept { return num_hashes_; }
    std::size_t count() const noexcept { return count_; }
    std::size_t memory_bytes() const noexcept { return words_.size() * sizeof(std::uint64_t); }
    const std::vector<std::uint64_t>& words() const noexcept { return words_; }

private:
    // Kirsch-Mitzenmacher: derive k probes from two hashes rather than computing
    // k independent ones, with no measurable loss in false-positive rate.
    std::pair<std::uint64_t, std::uint64_t> hashes(std::span<const std::byte> key) const noexcept {
        const std::uint64_t h1 = mix(fnv1a(key, 0xcbf29ce484222325ULL));
        // Forced odd so the probe stride is coprime with any power-of-two span
        // and can never be zero (which would make all k probes the same bit).
        const std::uint64_t h2 = mix(fnv1a(key, 0x9e3779b97f4a7c15ULL)) | 1ULL;
        return {h1, h2};
    }

    std::size_t capacity_;
    double error_rate_;
    std::size_t num_bits_ = 0;
    std::size_t num_hashes_ = 0;
    std::size_t count_ = 0;
    std::vector<std::uint64_t> words_;
};

std::span<const std::byte> as_bytes(const nb::bytes& value) {
    return {reinterpret_cast<const std::byte*>(value.c_str()), value.size()};
}

std::span<const std::byte> as_bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

// Shared by add_all, which sees heterogeneous Python objects rather than a
// resolved overload.
void add_any(BloomFilter& filt, nb::handle item) {
    if (nb::isinstance<nb::bytes>(item)) {
        filt.add(as_bytes(nb::cast<nb::bytes>(item)));
    } else {
        filt.add(as_bytes(nb::cast<std::string_view>(item)));
    }
}

// (capacity, error_rate, num_bits, num_hashes, count, raw bit array)
using State = std::tuple<std::size_t, double, std::size_t, std::size_t, std::size_t, nb::bytes>;

}  // namespace

NB_MODULE(_core, m) {
    m.doc() = "A Bloom filter in C++20, bound with nanobind.";
    m.attr("__version__") = BLOOMDEMO_VERSION;

    // std::invalid_argument maps to Python's ValueError automatically; nanobind
    // installs that translation for the standard exception hierarchy.
    nb::class_<BloomFilter>(m, "BloomFilter")
        .def(nb::init<std::size_t, double>(), "capacity"_a, "error_rate"_a = 0.01,
             "Create a filter sized to hold `capacity` items at roughly "
             "`error_rate` false positives.")

        // bytes first: registered overloads are tried in order, and bytes is the
        // exact match we want rather than letting it fall through to str.
        .def("add", [](BloomFilter& self, const nb::bytes& key) { self.add(as_bytes(key)); },
             "key"_a, "Add a key to the filter.")
        .def("add", [](BloomFilter& self, std::string_view key) { self.add(as_bytes(key)); },
             "key"_a)

        .def("add_all",
             [](BloomFilter& self, const nb::iterable& items) {
                 for (nb::handle item : items) {
                     add_any(self, item);
                 }
             },
             "items"_a,
             "Add every key from an iterable. The loop runs in C++, so this is "
             "the cheap way to fill a filter.")

        .def("__contains__",
             [](const BloomFilter& self, const nb::bytes& key) {
                 return self.contains(as_bytes(key));
             },
             "key"_a)
        .def("__contains__",
             [](const BloomFilter& self, std::string_view key) {
                 return self.contains(as_bytes(key));
             },
             "key"_a)

        .def("union", &BloomFilter::union_with, "other"_a,
             "Return a new filter containing everything from both. Raises "
             "ValueError if the parameters differ.")
        .def("__or__", &BloomFilter::union_with, "other"_a, nb::is_operator())

        .def("__len__", &BloomFilter::count,
             "Number of items added, counted exactly — not an estimate.")
        .def("__repr__", &BloomFilter::repr)

        .def_prop_ro("capacity", &BloomFilter::capacity, "Items the filter was sized for.")
        .def_prop_ro("error_rate", &BloomFilter::error_rate, "Target false-positive rate.")
        .def_prop_ro("num_bits", &BloomFilter::num_bits, "Bits in the array (m).")
        .def_prop_ro("num_hashes", &BloomFilter::num_hashes, "Probes per key (k).")
        .def_prop_ro("memory_bytes", &BloomFilter::memory_bytes,
                     "Bytes occupied by the bit array.")

        // Hashing is seed-free by construction, so a pickled filter still works
        // in a new process. Python's hash() would not survive PYTHONHASHSEED.
        .def("__getstate__",
             [](const BloomFilter& self) {
                 const auto& words = self.words();
                 // The blob is the raw word array, so it is host-endian. Fine for
                 // process-to-process reuse; byte-swap here if you ever ship
                 // pickles between architectures.
                 return State{self.capacity(), self.error_rate(),
                              self.num_bits(), self.num_hashes(), self.count(),
                              nb::bytes(reinterpret_cast<const char*>(words.data()),
                                        words.size() * sizeof(std::uint64_t))};
             })
        .def("__setstate__", [](BloomFilter& self, const State& state) {
            const nb::bytes& blob = std::get<5>(state);
            std::vector<std::uint64_t> words(blob.size() / sizeof(std::uint64_t));
            std::memcpy(words.data(), blob.c_str(), blob.size());
            new (&self) BloomFilter(std::get<0>(state), std::get<1>(state), std::get<2>(state),
                                    std::get<3>(state), std::get<4>(state), std::move(words));
        });
}
