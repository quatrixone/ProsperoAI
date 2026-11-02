# SPDX-License-Identifier: GPL-3.0-or-later
# libstdc++ eagerly instantiates pair<PickleValue,PickleValue> while PickleValue
# is incomplete. Define its special members after the complete class, preserving
# the upstream parser and ownership behavior. Keep the immutable checkout intact.
set(pickle_source "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src/model_io/pickle_io.cpp")
file(READ "${pickle_source}" pickle_content)
set(pickle_start "struct PickleValue {")
set(pickle_end "    PickleTensorInfo tensor;\n};")
string(FIND "${pickle_content}" "${pickle_start}" pickle_start_at)
string(FIND "${pickle_content}" "${pickle_end}" pickle_end_at)
if(pickle_start_at LESS 0 OR pickle_end_at LESS 0)
    message(FATAL_ERROR "Pinned SD PickleValue layout changed; review the portability patch")
endif()
string(REPLACE "${pickle_start}" "${pickle_start}
    PickleValue();
    ~PickleValue();
    PickleValue(const PickleValue&);
    PickleValue(PickleValue&&) noexcept;
    PickleValue& operator=(const PickleValue&);
    PickleValue& operator=(PickleValue&&) noexcept;" pickle_content "${pickle_content}")
string(REPLACE "${pickle_end}" "${pickle_end}
PickleValue::PickleValue() = default;
PickleValue::~PickleValue() = default;
PickleValue::PickleValue(const PickleValue&) = default;
PickleValue::PickleValue(PickleValue&&) noexcept = default;
PickleValue& PickleValue::operator=(const PickleValue&) = default;
PickleValue& PickleValue::operator=(PickleValue&&) noexcept = default;" pickle_content "${pickle_content}")
set(patched_pickle "${CMAKE_CURRENT_BINARY_DIR}/sd-patches/pickle_io.cpp")
if(EXISTS "${patched_pickle}")
    file(READ "${patched_pickle}" previous_pickle)
else()
    set(previous_pickle "")
endif()
if(NOT previous_pickle STREQUAL pickle_content)
    file(WRITE "${patched_pickle}" "${pickle_content}")
endif()
get_target_property(sd_sources stable-diffusion SOURCES)
list(REMOVE_ITEM sd_sources "${pickle_source}")
list(APPEND sd_sources "${patched_pickle}")
set_property(TARGET stable-diffusion PROPERTY SOURCES "${sd_sources}")
set_source_files_properties("${patched_pickle}"
    DIRECTORY "${PROS_ROOT}/.deps/stable-diffusion-vulkan"
    PROPERTIES INCLUDE_DIRECTORIES
    "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src/model_io")

# libstdc++ returns std::u8string from path::u8string in C++20, while this
# SD source returns it as std::string. Its model paths are UTF-8 on PS5.
set(safetensors_source "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src/model_io/safetensors_io.cpp")
file(READ "${safetensors_source}" safetensors_content)
string(FIND "${safetensors_content}" ".u8string()" safetensors_u8string_at)
if(safetensors_u8string_at GREATER_EQUAL 0)
    string(REPLACE ".u8string()" ".string()" safetensors_content "${safetensors_content}")
    set(patched_safetensors "${CMAKE_CURRENT_BINARY_DIR}/sd-patches/safetensors_io.cpp")
    if(EXISTS "${patched_safetensors}")
        file(READ "${patched_safetensors}" previous_safetensors)
    else()
        set(previous_safetensors "")
    endif()
    if(NOT previous_safetensors STREQUAL safetensors_content)
        file(WRITE "${patched_safetensors}" "${safetensors_content}")
    endif()
    get_target_property(sd_sources stable-diffusion SOURCES)
    list(REMOVE_ITEM sd_sources "${safetensors_source}")
    list(APPEND sd_sources "${patched_safetensors}")
    set_property(TARGET stable-diffusion PROPERTY SOURCES "${sd_sources}")
    set_source_files_properties("${patched_safetensors}"
        DIRECTORY "${PROS_ROOT}/.deps/stable-diffusion-vulkan"
        PROPERTIES INCLUDE_DIRECTORIES
        "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src/model_io")
endif()

# On PS5 the Vulkan memory-budget query can report a free-byte sentinel larger than
# the heap total. Use the known heap total, then subtract the model manager's tracked
# resident allocations below, instead of treating all capacity as exhausted.
set(model_manager_source "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src/model_manager.cpp")
file(READ "${model_manager_source}" model_manager_content)
set(invalid_budget_check [==[        if (total_bytes > 0 && free_bytes > total_bytes && sd_backend_is(backend, "Vulkan")) {
            return size_t{0};
        }]==])
set(valid_budget_check [==[        if (total_bytes > 0 && free_bytes > total_bytes && sd_backend_is(backend, "Vulkan")) {
            free_bytes = total_bytes;
        }]==])
string(FIND "${model_manager_content}" "${valid_budget_check}" model_manager_patch_at)
if(model_manager_patch_at LESS 0)
    string(FIND "${model_manager_content}" "if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_IGPU)"
        integrated_budget_device_check_at)
    string(FIND "${model_manager_content}" "free_bytes                  = total_bytes;"
        integrated_budget_clamp_at)
    string(FIND "${model_manager_content}" "result.requires_reclamation = true;"
        integrated_budget_reclaim_at)
    if(integrated_budget_device_check_at LESS 0 OR integrated_budget_clamp_at LESS 0 OR
       integrated_budget_reclaim_at LESS 0)
        string(FIND "${model_manager_content}" "${invalid_budget_check}" model_manager_patch_at)
    endif()
    if(model_manager_patch_at LESS 0 AND
       (integrated_budget_device_check_at LESS 0 OR integrated_budget_clamp_at LESS 0 OR
        integrated_budget_reclaim_at LESS 0))
        message(FATAL_ERROR "Pinned SD Vulkan memory-budget logic changed; review the PS5 fallback patch")
    endif()
    if(model_manager_patch_at GREATER_EQUAL 0)
        string(REPLACE "${invalid_budget_check}" "${valid_budget_check}"
            model_manager_content "${model_manager_content}")
    endif()
endif()
set(patched_model_manager "${CMAKE_CURRENT_BINARY_DIR}/sd-patches/model_manager.cpp")
if(EXISTS "${patched_model_manager}")
    file(READ "${patched_model_manager}" previous_model_manager)
else()
    set(previous_model_manager "")
endif()
if(NOT previous_model_manager STREQUAL model_manager_content)
    file(WRITE "${patched_model_manager}" "${model_manager_content}")
endif()
get_target_property(sd_sources stable-diffusion SOURCES)
list(REMOVE_ITEM sd_sources "${model_manager_source}")
list(APPEND sd_sources "${patched_model_manager}")
set_property(TARGET stable-diffusion PROPERTY SOURCES "${sd_sources}")
set_source_files_properties("${patched_model_manager}"
    DIRECTORY "${PROS_ROOT}/.deps/stable-diffusion-vulkan"
    PROPERTIES INCLUDE_DIRECTORIES
    "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src")
