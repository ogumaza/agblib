// SPDX-License-Identifier: MIT

// What the tests of Frontline Studios' XM-like player share: the driver's tables in the test cartridge.

#pragma once

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/testing/frontline_xm_cart.h"

namespace agblib::testing::frontline_xm_tests
{

// Returns a driver with the tables of the test cartridge, its rate's index, and no module or song.
inline frontline_xm::DriverInfo TestDriver()
{
    namespace cart = frontline_xm_cart;
    using agb::kRomBase;

    frontline_xm::DriverInfo info;
    frontline_xm::Library& lib = info.library;
    lib.steps = kRomBase + cart::kSteps;
    lib.periods = kRomBase + cart::kPeriods;
    lib.step_rates = kRomBase + cart::kStepRates;
    lib.frame_samples = kRomBase + cart::kFrameSamples;
    lib.rates = kRomBase + cart::kRates;
    lib.silence = kRomBase + cart::kSilence;
    lib.waves = {kRomBase + cart::kWaves, kRomBase + cart::kWaves + 256, kRomBase + cart::kWaves + 512};
    info.rate_index = cart::kRateIndex;

    return info;
}

} // namespace agblib::testing::frontline_xm_tests
