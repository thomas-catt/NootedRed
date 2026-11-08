// Automatic runtime Metal compatibility patches. See docs/Chromium-Metal.md.
#pragma once
#include <Headers/kern_patcher.hpp>

namespace MetalPatch { void init(KernelPatcher& patcher); }
