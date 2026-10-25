#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Battery-protection threshold persistence for Unsleep.
//
// Only the threshold is persisted, in a minimal application-specific settings
// file next to the executable. The parser accepts exactly one
// "threshold=<10|20|30>" line, so a corrupted or hand-edited file falls back to
// the product default instead of inventing behaviour. File access lives here
// rather than in the core model; this module never calls Windows APIs.
namespace unsleep {

inline constexpr int default_battery_threshold = 20;
inline constexpr std::array<int, 3> battery_thresholds{10, 20, 30};
inline constexpr wchar_t settings_file_name[] = L"unsleep.settings";

[[nodiscard]] bool is_valid_threshold(int percent) noexcept;

enum class ThresholdLoadStatus { loaded, missing, invalid, read_failed };

struct ThresholdLoad {
    int threshold{default_battery_threshold};
    ThresholdLoadStatus status{ThresholdLoadStatus::missing};
};

enum class ThresholdSaveStatus { saved, invalid_value, write_failed };

// Returns the threshold encoded by the text, or nothing when the text is not
// exactly one well-formed line. Callers fall back to the default on nothing.
[[nodiscard]] std::optional<int> parse_threshold(std::string_view text) noexcept;
[[nodiscard]] std::string format_threshold(int percent);

[[nodiscard]] ThresholdLoad load_threshold(const std::filesystem::path& file) noexcept;
[[nodiscard]] ThresholdSaveStatus save_threshold(const std::filesystem::path& file, int percent) noexcept;

// Settings live next to the executable; no registry or AppData writes.
[[nodiscard]] std::filesystem::path settings_path_for(const std::filesystem::path& executable);

} // namespace unsleep
