# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(services/clean)
add_subdirectory("services/bad")
install(TARGETS clean_node) # module-hook: install rule from services/README.md
# A comment naming bad_lib is not a reference.
target_link_libraries(core_thing PRIVATE bad_lib) # rule core-cmake-target
