// Copyright (c) 2026 Maged Michael
// See LICENSES for licensing terms.

#include "mm_hp.hpp"

namespace mm_hp_detail {

constinit hp_domain g_domain{};
hp_domain_exit_guard g_domain_exit_guard;

} // namespace mm_hp_detail
