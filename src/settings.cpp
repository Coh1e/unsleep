#include "unsleep/settings.hpp"

#include <fstream>
#include <system_error>

namespace unsleep {
namespace {

constexpr std::string_view key_prefix = "threshold=";
constexpr std::size_t max_file_bytes = 64;

// Reads the whole file. A missing read permission, a directory, or any other
// open/read error is reported through read_ok instead of being hidden.
std::string read_text(const std::filesystem::path& file, bool& read_ok, bool& too_large) {
    read_ok = false;
    too_large = false;
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    std::string text;
    char character{};
    while (input.get(character)) {
        if (text.size() >= max_file_bytes) {
            too_large = true;
            return text;
        }
        text.push_back(character);
    }
    read_ok = input.eof();
    return text;
}

} // namespace

bool is_valid_threshold(int percent) noexcept {
    for (const int candidate : battery_thresholds) {
        if (candidate == percent) return true;
    }
    return false;
}

std::optional<int> parse_threshold(std::string_view text) noexcept {
    // The minimal format is one line with an optional LF or CRLF terminator.
    if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    if (!text.starts_with(key_prefix)) return std::nullopt;
    const auto value = text.substr(key_prefix.size());
    for (const int candidate : battery_thresholds) {
        if (value == std::to_string(candidate)) return candidate;
    }
    return std::nullopt;
}

std::string format_threshold(int percent) {
    return std::string(key_prefix) + std::to_string(percent) + "\n";
}

ThresholdLoad load_threshold(const std::filesystem::path& file) noexcept {
    ThresholdLoad result;
    if (file.empty()) {
        result.status = ThresholdLoadStatus::read_failed;
        return result;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        // An unreadable location is a read failure, not an absent file.
        result.status = ThresholdLoadStatus::read_failed;
        return result;
    }
    if (!exists) {
        result.status = ThresholdLoadStatus::missing;
        return result;
    }
    if (!std::filesystem::is_regular_file(file, error) || error) {
        result.status = ThresholdLoadStatus::read_failed;
        return result;
    }
    bool read_ok = false;
    bool too_large = false;
    const auto text = read_text(file, read_ok, too_large);
    if (!read_ok) {
        result.status = ThresholdLoadStatus::read_failed;
        return result;
    }
    if (too_large) {
        result.status = ThresholdLoadStatus::invalid;
        return result;
    }
    const auto parsed = parse_threshold(text);
    if (!parsed) {
        result.status = ThresholdLoadStatus::invalid;
        return result;
    }
    result.threshold = *parsed;
    result.status = ThresholdLoadStatus::loaded;
    return result;
}

ThresholdSaveStatus save_threshold(const std::filesystem::path& file, int percent) noexcept {
    if (!is_valid_threshold(percent)) return ThresholdSaveStatus::invalid_value;
    if (file.empty()) return ThresholdSaveStatus::write_failed;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return ThresholdSaveStatus::write_failed;
    const std::string text = format_threshold(percent);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        output.close();
        return ThresholdSaveStatus::write_failed;
    }
    output.close();
    return output.fail() ? ThresholdSaveStatus::write_failed : ThresholdSaveStatus::saved;
}

std::filesystem::path settings_path_for(const std::filesystem::path& executable) {
    return executable.parent_path() / settings_file_name;
}

} // namespace unsleep
