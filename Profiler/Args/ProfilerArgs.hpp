#pragma once

#include <concepts>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../ProfilerFeatures.hpp"

namespace Profiler {
    struct ProfilerArgs {
        static constexpr int CheckVal{123};
        static constexpr std::string_view EnvPrefix{"SIGMA_ENV_"};

        std::filesystem::path ShimDir{};

        // Provider flags — always present regardless of which backends were compiled in.
        // The CLI parser owns defaults; to_env/from_env are guarded by feature macros
        // because those paths are only meaningful when the corresponding backend exists.

        // TODO: this should be in feature-guarded wrapper objects
        bool LoadIbtrace{true};
        bool LoadMlx5dv{true};
        bool LoadCupti{true};
        bool LoadLibfabric{true};
        bool LoadUcx{true};

        bool EnableDoorbellHijack{false};

        int Check{CheckVal};

        [[nodiscard]] std::vector<std::string> to_env() const {
            std::vector<std::string> env;
            env.push_back(make_env_entry("ShimPath", ShimDir));
            if constexpr (Features::HasRdmaCore) {
                env.push_back(make_env_entry("LoadIbtrace", LoadIbtrace));
                env.push_back(make_env_entry("LoadMlx5dv", LoadMlx5dv));
                env.push_back(make_env_entry("EnableDoorbellHijack", EnableDoorbellHijack));
            }

            if constexpr (Features::HasCUDA) {
                env.push_back(make_env_entry("LoadCupti", LoadCupti));
            }

            if constexpr (Features::HasLibfabric) {
                env.push_back(make_env_entry("LoadLibfabric", LoadLibfabric));
            }

            if constexpr (Features::HasUCX) {
                env.push_back(make_env_entry("LoadUcx", LoadUcx));
            }

            env.push_back(make_env_entry("Check", Check));
            return env;
        }

        [[nodiscard]] static ProfilerArgs from_env() {
            return {
                    .ShimDir{parse_env<std::filesystem::path>("ShimPath", {})},
                    .LoadIbtrace = parse_env("LoadIbtrace", true),
                    .LoadMlx5dv = parse_env("LoadMlx5dv", true),
                    .LoadCupti = parse_env("LoadCupti", true),
                    .LoadLibfabric = parse_env("LoadLibfabric", false),
                    .LoadUcx = parse_env("LoadUcx", false),
                    .EnableDoorbellHijack = parse_env("EnableDoorbellHijack", false),
                    .Check = parse_env("Check", 0),
            };
        }

    private:
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
        [[nodiscard]] static T parse_env(const std::string_view name, T default_value) {
            const auto env_name = std::string{EnvPrefix} + std::string{name};

            const char* val = std::getenv(env_name.c_str());
            if (val == nullptr) {
                return default_value;
            }

            const std::string_view val_str{val};
            if (val_str.length() < 3) {
                return default_value;
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
