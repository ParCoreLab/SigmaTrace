#pragma once

#include <concepts>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Very ugly
#define INTERNAL_PARSE_ENV_FIELD(field_name) .field_name = parse_env<decltype(field_name)>(#field_name)
#define INTERNAL_CONVERT_FIELD_ENV(field_name) make_env_entry(#field_name, field_name)

namespace Profiler {
    enum class ProfilerTraceMode {
        Error = 0,
        Aggregate,
        Simple,
        Full,
    };

    inline std::istream& operator>>(std::istream& is, ProfilerTraceMode& target) {
        std::string value;
        is >> value;
        if (is.fail()) {
            is.setstate(std::ios::failbit);
            return is;
        }

        if (value == "simple" || value == "default") {
            target = ProfilerTraceMode::Simple;
        } else if (value == "aggregate") {
            target = ProfilerTraceMode::Aggregate;
        } else if (value == "full") {
            target = ProfilerTraceMode::Full;
        } else {
            is.setstate(std::ios::failbit);
        }
        return is;
    }

    inline std::ostream& operator<<(std::ostream& os, const ProfilerTraceMode& mode) {
        switch (mode) {
            case ProfilerTraceMode::Simple:
                os << "simple";
                break;
            case ProfilerTraceMode::Aggregate:
                os << "aggregate";
                break;
            case ProfilerTraceMode::Full:
                os << "full";
                break;
            default:
                os << "error";
                break;
        }
        return os;
    }

    struct ProfilerArgs {
        static constexpr int CheckVal{123};
        static constexpr std::string_view EnvPrefix{"SIGMA_ENV_"};

        std::filesystem::path ShimDirectory{};

        // Provider flags always present regardless of which backends were compiled in.
        // TODO: these should be in feature-guarded wrapper objects
        bool LoadIbtrace;
        bool LoadMlx5dv;
        bool LoadCupti;
        bool LoadLibfabric;
        bool LoadUcx;

        bool EnableDoorbellHijack;

        // Output
        std::string OutputDirectory;
        bool OutputOverwrite;

        ProfilerTraceMode TraceMode;

        int Check{CheckVal};

        [[nodiscard]] std::array<std::string, 11> to_env() const {
            return {
                    INTERNAL_CONVERT_FIELD_ENV(ShimDirectory),        //
                    INTERNAL_CONVERT_FIELD_ENV(LoadIbtrace),          //
                    INTERNAL_CONVERT_FIELD_ENV(LoadMlx5dv),           //
                    INTERNAL_CONVERT_FIELD_ENV(EnableDoorbellHijack), //
                    INTERNAL_CONVERT_FIELD_ENV(LoadCupti),            //
                    INTERNAL_CONVERT_FIELD_ENV(LoadLibfabric),        //
                    INTERNAL_CONVERT_FIELD_ENV(LoadUcx),              //
                    INTERNAL_CONVERT_FIELD_ENV(OutputDirectory),      //
                    INTERNAL_CONVERT_FIELD_ENV(OutputOverwrite),      //
                    INTERNAL_CONVERT_FIELD_ENV(TraceMode),            //
                    INTERNAL_CONVERT_FIELD_ENV(Check),                //

            };
        }

        // TODO: remove default values
        [[nodiscard]] static ProfilerArgs from_env() {
            return {
                    INTERNAL_PARSE_ENV_FIELD(ShimDirectory),        //
                    INTERNAL_PARSE_ENV_FIELD(LoadIbtrace),          //
                    INTERNAL_PARSE_ENV_FIELD(LoadMlx5dv),           //
                    INTERNAL_PARSE_ENV_FIELD(LoadCupti),            //
                    INTERNAL_PARSE_ENV_FIELD(LoadLibfabric),        //
                    INTERNAL_PARSE_ENV_FIELD(LoadUcx),              //
                    INTERNAL_PARSE_ENV_FIELD(EnableDoorbellHijack), //
                    INTERNAL_PARSE_ENV_FIELD(OutputDirectory),      //
                    INTERNAL_PARSE_ENV_FIELD(OutputOverwrite),      //
                    INTERNAL_PARSE_ENV_FIELD(TraceMode),            //
                    INTERNAL_PARSE_ENV_FIELD(Check),                //
            };
        }

    private:
        [[nodiscard]] static std::string value_to_string(const ProfilerTraceMode& mode) {
            switch (mode) {
                case ProfilerTraceMode::Simple:
                    return "simple";
                case ProfilerTraceMode::Aggregate:
                    return "aggregate";
                case ProfilerTraceMode::Full:
                    return "full";
                default:
                    return "error";
            }
        }

        [[nodiscard]] static std::string value_to_string(std::string value) {
            return value;
        }

        [[nodiscard]] static std::string value_to_string(const bool value) {
            return value ? "1" : "0";
        }

        template<std::integral T>
        [[nodiscard]] static std::string value_to_string(T value) {
            return std::to_string(value);
        }

        template<typename T>
        [[nodiscard]] static std::string make_env_entry(std::string_view name, T value) {
            return std::string{EnvPrefix} + std::string{name} + "=\"" + value_to_string(std::move(value)) + "\"";
        }

        template<typename T>
        [[nodiscard]] static T parse_env(const std::string_view name) {
            const auto env_name = std::string{EnvPrefix} + std::string{name};

            const char* val = std::getenv(env_name.c_str());
            if (val == nullptr) {
                throw std::runtime_error("Missing value for " + env_name);
            }

            const std::string_view val_str{val};
            if (val_str.length() < 3) {
                throw std::runtime_error("Empty quoted value in " + env_name);
            }

            if (!(val_str.starts_with('"') && val_str.ends_with('"'))) {
                throw std::runtime_error("Invalid value from " + env_name + ": " + std::string{val});
            }

            // We need a null terminated string after this point
            std::string val_str_unwrapped{val_str.begin() + 1, val_str.end() - 1};

            if constexpr (std::is_same_v<T, bool>) {
                if (val_str_unwrapped == "1") {
                    return true;
                }

                if (val_str_unwrapped == "0") {
                    return false;
                }

                throw std::runtime_error("Invalid value from " + env_name + ": " + std::string{val});
            }

            if constexpr (std::is_integral_v<T>) {
                if constexpr (std::is_signed_v<T>) {
                    auto parsed_value = std::stoll(val_str_unwrapped);
                    return static_cast<T>(parsed_value);
                } else /* if constexpr (!std::is_signed_v<T>) */ {
                    auto parsed_value = std::stoull(val_str_unwrapped);
                    return static_cast<T>(parsed_value);
                }
            } else if constexpr (std::constructible_from<std::string, T>) {
                return val_str_unwrapped;
            } else if constexpr (requires(std::istream& is, T v) { is >> v; }) {
                T parsed_val{};
                std::istringstream iss{val_str_unwrapped};
                if (!(iss >> parsed_val)) {
                    throw std::runtime_error("Invalid value from " + env_name + ": " + std::string{val});
                }

                return parsed_val;
            } else {
                // Failsafe if the class has unsupported types
                // DR on some compilers (<= GCC 12)
                // https://www.reddit.com/r/cpp_questions/comments/1it55nq/gcc_114clang_14_static_assert_hits_even_without/
                static_assert(sizeof(T) && false);
            }

            throw std::runtime_error("Invalid value from " + env_name + ": " + std::string{val});
        }
    };
} // namespace Profiler
