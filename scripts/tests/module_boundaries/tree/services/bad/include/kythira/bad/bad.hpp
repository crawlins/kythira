// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once
// rule other-module
#include <kythira/clean/clean.hpp>
// rule impl-header (not the composition root)
#include <raft/thing_impl.hpp>
// rule core-tests (library header, test fabric not on its include path)
#include <fabric.hpp>
// rule core-cmd
#include "../../../../../cmd/tool/tool.hpp"
#include <raft/public.hpp>
