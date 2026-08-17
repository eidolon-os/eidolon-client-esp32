#pragma once

// Host-test stand-in for the build-time sdkconfig.
//
// The device profile is decided at compile time from Kconfig, so a host test
// that includes it has to pick a profile too. It picks half duplex: the profile
// that assumes least about the hardware, and the only one a board with no
// declared AEC reference may hold — the header refuses to compile full duplex
// without one. These tests are about the wire protocol, which every profile
// shares. A test that needs a different profile should define the symbols
// itself before including the header rather than change the default here, so no
// other test's meaning moves with it.
#define CONFIG_EIDOLON_INTERACTION_MODE_PTT 0
#define CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX 1
